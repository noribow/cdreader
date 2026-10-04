#include "rip_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <map>
#include <memory>
#include <sstream>

#include "cdreader/audio_writer.h"
#include "cdreader/crc32.h"
#include "cdreader/file_naming.h"

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "0.1.0"
#endif

namespace cdr {

namespace {

std::string fileExtension(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

}  // namespace

// --- AccurateRipReport -------------------------------------------------------

int AccurateRipReport::accurateTracks() const {
    int n = 0;
    for (const AccurateRipTrackResult& t : tracks) n += t.accurate() ? 1 : 0;
    return n;
}

int AccurateRipReport::tracksInDatabase() const {
    int n = 0;
    for (const AccurateRipTrackResult& t : tracks) n += t.inDatabase() ? 1 : 0;
    return n;
}

std::vector<std::string> AccurateRipReport::logLines() const {
    std::vector<std::string> lines;
    const bool found = status == Status::Found;
    lines.push_back("AccurateRip (disc id " + id.toString() + ")" +
                    (found ? ", " + std::to_string(pressings) + " pressing(s) in database" : std::string()));
    std::map<std::string, int> byVersion;  // accurate tracks per matched checksum version
    for (const AccurateRipTrackResult& r : tracks) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Track %02d  v1 %s  v2 %s  ", r.track, hex32(r.v1).c_str(), hex32(r.v2).c_str());
        lines.push_back(buf + (found ? r.describe() : std::string()));
        for (const AccurateRipPressingMatch& p : r.pressings) {
            std::snprintf(buf, sizeof buf, "          pressing %d: %s  confidence %3d  %s", p.pressing,
                          hex32(p.checksum).c_str(), p.confidence,
                          p.version == 2 ? "v2 match" : p.version == 1 ? "v1 match" : "no match");
            lines.push_back(buf);
        }
        if (r.accurate()) ++byVersion[r.matchedVersion()];
    }

    switch (status) {
        case Status::Disabled:
            lines.push_back("AccurateRip: lookup disabled");
            break;
        case Status::NotFound:
            lines.push_back("AccurateRip: this disc is not in the database (no rips submitted yet)");
            break;
        case Status::Error:
            lines.push_back("AccurateRip: lookup failed: " + error);
            break;
        case Status::Found: {
            std::string versions;
            for (const auto& [version, count] : byVersion)
                versions += (versions.empty() ? "" : ", ") + version + ": " + std::to_string(count);
            const int accurate = accurateTracks();
            const int inDatabase = tracksInDatabase();
            lines.push_back("AccurateRip: " + std::to_string(accurate) + " of " + std::to_string(tracks.size()) +
                            " track(s) accurately ripped" + (versions.empty() ? "" : " (" + versions + ")") + ", " +
                            std::to_string(inDatabase) + " track(s) in database");
            if (accurate == 0 && inDatabase > 0)
                lines.push_back("Hint: no track matched. Check the read offset.");
            break;
        }
    }
    return lines;
}

// --- RipSession --------------------------------------------------------------

const DriveInfo& RipSession::driveInfo() {
    if (!driveInfo_) driveInfo_ = drive_.inquiry();
    return *driveInfo_;
}

const std::string& RipSession::driveName() {
    if (!driveName_) driveName_ = driveInfo().displayName();
    return *driveName_;
}

const DriveOffsetDbMatch& RipSession::lookupDriveOffsetDb(HttpClient* http) {
    namespace fs = std::filesystem;
    DriveOffsetDbCache cache;
    if (!driveOffsetDbCache_.empty()) {
        std::error_code ec;
        const fs::file_time_type written = fs::last_write_time(driveOffsetDbCache_, ec);
        if (!ec) {
            std::ifstream in(driveOffsetDbCache_, std::ios::binary);
            if (in) {
                cache.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                cache.present = !in.bad();
                cache.ageSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - written).count();
            }
        }
    }
    const DriveOffsetDbLoad load = loadDriveOffsetDb(http, cache);
    if (load.shouldStore() && !driveOffsetDbCache_.empty()) {
        // Write next to it and rename, so that a failure never leaves half a file.
        std::error_code ec;
        fs::create_directories(driveOffsetDbCache_.parent_path(), ec);
        fs::path temp = driveOffsetDbCache_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(load.bytes.data(), std::streamsize(load.bytes.size()));
            out.flush();
            if (!out) ec = std::make_error_code(std::errc::io_error);
        }
        if (!ec) fs::rename(temp, driveOffsetDbCache_, ec);
        if (ec) fs::remove(temp, ec);
    }
    try {
        driveOffsetDb_ = matchDriveOffsetDb(load, driveInfo());
    } catch (const std::exception& e) {
        driveOffsetDb_ = {};
        driveOffsetDb_.status = DriveOffsetDbMatch::Status::Unavailable;
        driveOffsetDb_.error = e.what();
    }
    return driveOffsetDb_;
}

const OffsetDetection& RipSession::detectReadOffset(HttpClient* http, OffsetDetectOptions options) {
    const Toc& t = toc();
    cancelled_ = false;
    options.cancelled = [this] { return cancelled_.load(); };
    detection_.reset();
    options.driveDatabase = lookupDriveOffsetDb(http);
    if (http == nullptr) {
        OffsetDetection d;
        d.id = AccurateRipDiscId::fromToc(t);
        d.maxOffset = options.maxOffset;
        d.driveDatabase = options.driveDatabase;
        d.status = OffsetDetection::Status::LookupFailed;
        d.error = "AccurateRip lookup disabled";
        detection_ = std::move(d);
        return *detection_;
    }
    detection_ = cdr::detectReadOffset(drive_, t, *http, options);
    return *detection_;
}

const Toc& RipSession::readToc() {
    toc_.reset();
    toc_ = drive_.readToc();
    cddb_ = {};
    cddbLookedUp_ = false;
    album_ = {};
    album_.discId = hex32(toc_->cddbId());
    discCodes_ = {};
    gaps_ = gapsFromToc(*toc_);
    detectedCache_.reset();
    ripped_.clear();
    image_.reset();
    accurateRip_ = {};
    accurateRipChecked_ = false;
    detection_.reset();
    return *toc_;
}

const Toc& RipSession::toc() {
    if (!toc_) return readToc();
    return *toc_;
}

const CddbLookupResult& RipSession::lookupCddb(HttpClient* http, const CddbSettings& settings) {
    const Toc& t = toc();
    cddbSettings_ = settings;
    cddbLookedUp_ = true;
    cddb_ = {};
    if (!settings.enabled || http == nullptr) {
        cddbSettings_.enabled = false;
        cddb_.error = "disabled";
    } else {
        try {
            cddb_ = cdr::lookupCddb(*http, t, settings.options);
        } catch (const std::exception& e) {  // lookupCddb does not throw; be safe anyway
            cddb_ = {};
            cddb_.error = e.what();
        }
    }
    album_ = cddb_.found ? cddb_.album : AlbumMetadata{};
    album_.discId = hex32(t.cddbId());
    discCodes_.applyTo(album_);
    return cddb_;
}

const DiscCodes& RipSession::readDiscCodes() {
    const Toc& t = toc();
    discCodes_ = cdr::readDiscCodes(drive_, t.tracks);
    discCodes_.applyTo(album_);
    return discCodes_;
}

const DiscGaps& RipSession::detectGaps() {
    const Toc& t = toc();
    gaps_ = cdr::detectGaps(drive_, t);
    return gaps_;
}

TrackMetadata RipSession::trackMetadata(int number) { return album_.forTrack(number, toc().lastTrack); }

std::string RipSession::trackLabel(int number) {
    const TrackMetadata m = trackMetadata(number);
    if (m.title.empty()) return {};
    return m.artist != album_.artist && !m.artist.empty() ? m.artist + " / " + m.title : m.title;
}

std::string RipSession::albumDirectoryName() const {
    AlbumMetadata album = album_;
    if (album.discId.empty() && toc_) album.discId = hex32(toc_->cddbId());
    return cdr::albumDirectoryName(album);
}

std::string RipSession::trackFileName(int number, const std::string& format) {
    const std::unique_ptr<AudioWriter> writer = createAudioWriter(format);
    if (!writer) throw std::invalid_argument("unknown format '" + format + "'");
    return trackFileBaseName(trackMetadata(number)) + "." + writer->extension();
}

void RipSession::beginRip(const RipSettings& settings) {
    if (!createAudioWriter(settings.format, settings.encoder))  // throws for invalid encoder settings
        throw std::invalid_argument("unknown format '" + settings.format + "'");
    // Far beyond any real drive (known offsets are within about +-3000 samples).
    if (std::abs(settings.options.readOffsetSamples) > int(100 * kSamplesPerSector))
        throw std::invalid_argument("read offset out of range");
    if (settings.options.maxRetries < 0) throw std::invalid_argument("negative retry count");
    settings_ = settings;
    // MODE SENSE on every rip: cheap, and the drive may have been swapped.
    std::optional<DriveCapabilities> caps;
    if (settings.useC2 || settings.cache != CacheSetting::None) caps = drive_.readCapabilities();
    c2_ = caps ? checkC2(*caps, settings.useC2) : C2Availability{};
    settings_.options.useC2 = c2_.usable();
    c2Fallback_.clear();
    // The cache timing test reads the disc for a few seconds: once per disc.
    if (settings.cache == CacheSetting::Auto && detectedCache_) {
        cacheCheck_ = *detectedCache_;
    } else {
        static const Toc noToc;  // None needs no TOC (and sends no command)
        const Toc& t = settings.cache == CacheSetting::None ? noToc : toc();
        cacheCheck_ = checkDriveCache(drive_, t, settings.cache, *clock_, caps ? &*caps : nullptr);
        if (settings.cache == CacheSetting::Auto) detectedCache_ = cacheCheck_;
    }
    settings_.options.cacheDefeat = cacheCheck_.method;
    settings_.options.flushSectors = cacheCheck_.flushSectors;
    cacheFallback_.clear();
    ripped_.clear();
    image_.reset();
    accurateRip_ = {};
    accurateRipChecked_ = false;
    cancelled_ = false;
    if (settings.readDiscCodes && !discCodes_.read) readDiscCodes();
    if (settings.detectGaps && gaps_.status == DiscGaps::Status::NotRun) detectGaps();
}

const RippedTrack& RipSession::ripTrack(int number, const std::filesystem::path& path,
                                        const Ripper::Progress& progress) {
    if (cancelled_) throw RipCancelled();
    const Toc& t = toc();
    const Track* track = t.findTrack(number);
    if (track == nullptr) throw std::invalid_argument("track " + std::to_string(number) + " is not on this disc");
    if (!track->isAudio) throw std::invalid_argument("track " + std::to_string(number) + " is not an audio track");

    std::unique_ptr<AudioWriter> writer = createAudioWriter(settings_.format, settings_.encoder);
    if (!writer) throw std::invalid_argument("unknown format '" + settings_.format + "'");
    const TrackMetadata metadata = trackMetadata(number);

    RippedTrack ripped;
    ripped.track = *track;
    ripped.fileName = trackFileBaseName(metadata) + "." + writer->extension();

    writer->open(path, metadata);
    AccurateRipChecksum ar = AccurateRipChecksum::forTrack(t, *track);
    Ripper ripper(drive_, t, settings_.options);
    ripped.result = ripper.ripTrack(
        *track,
        [&](const uint8_t* pcm, size_t bytes) {
            if (cancelled_) throw RipCancelled();
            writer->write(pcm, bytes);
            ar.update(pcm, bytes);
        },
        [&](uint32_t done, uint32_t total) {
            if (cancelled_) throw RipCancelled();
            if (progress) progress(done, total);
        });
    writer->close();
    takeFallbacks(ripper, ripped.result);
    ripped.accurateRipV1 = ar.v1();
    ripped.accurateRipV2 = ar.v2();

    // Per-track files after an image: the image's parts are no results of these.
    if (image_) {
        image_.reset();
        ripped_.clear();
    }
    // Ripping a track again replaces its earlier result.
    for (auto it = ripped_.begin(); it != ripped_.end(); ++it) {
        if (it->track.number == number) {
            ripped_.erase(it);
            break;
        }
    }
    ripped_.push_back(std::move(ripped));
    accurateRipChecked_ = false;
    return ripped_.back();
}

void RipSession::takeFallbacks(const Ripper& ripper, const TrackRipResult& result) {
    if (result.c2 && !ripper.c2Active() && settings_.options.useC2) {
        // The drive rejected C2 reads: plain reads for the rest of the disc.
        settings_.options.useC2 = false;
        c2Fallback_ = ripper.c2FallbackReason();
    }
    if (ripper.cacheDefeat() != settings_.options.cacheDefeat) {
        // The drive rejected FUA: flush for the rest of the rip.
        settings_.options.cacheDefeat = ripper.cacheDefeat();
        cacheFallback_ = ripper.cacheFallbackReason();
    }
}

DiscImagePlan RipSession::planImage(const std::vector<int>& numbers, const std::string& format) {
    const Toc& t = toc();
    const std::unique_ptr<AudioWriter> writer = createAudioWriter(format);
    if (!writer) throw std::invalid_argument("unknown format '" + format + "'");
    std::vector<Track> tracks;
    if (numbers.empty()) {
        for (const Track& track : t.tracks)
            if (track.isAudio) tracks.push_back(track);
        if (tracks.empty()) throw std::invalid_argument("no audio tracks on this disc");
    }
    for (int number : numbers) {
        const Track* track = t.findTrack(number);
        if (track == nullptr) throw std::invalid_argument("track " + std::to_string(number) + " is not on this disc");
        if (!track->isAudio)
            throw std::invalid_argument("track " + std::to_string(number) + " is not an audio track");
        tracks.push_back(*track);
    }
    // The HTOA is part of every image that starts with track 1, as in the CLI.
    return planDiscImage(tracks, t, album_, gaps_, true, writer->extension());
}

const RippedImage& RipSession::ripImage(const std::vector<int>& tracks, const std::filesystem::path& imagePath,
                                        const std::filesystem::path& cuePath, const ImageProgress& progress) {
    if (cancelled_) throw RipCancelled();
    const Toc& t = toc();
    RippedImage image;
    image.plan = planImage(tracks, settings_.format);
    const DiscImagePlan& plan = image.plan;

    std::vector<RippedTrack> parts;
    try {
        std::unique_ptr<AudioWriter> writer = createAudioWriter(settings_.format, settings_.encoder);
        if (!writer) throw std::invalid_argument("unknown format '" + settings_.format + "'");
        image.embeddedCueSheet = writer->canEmbedCueSheet();
        if (image.embeddedCueSheet) writer->setEmbeddedCueSheet(plan.embeddedCueSheet());
        writer->open(imagePath, album_.forTrack(0, t.lastTrack));

        Crc32 crc;
        uint32_t base = 0;  // sectors of the parts before this one
        Ripper ripper(drive_, t, settings_.options);
        for (const Track& part : plan.parts) {
            const bool htoa = part.number == 0;
            RippedTrack ripped;
            ripped.track = part;
            ripped.fileName = DiscImagePlan::partName(part);
            // No AccurateRip checksums for the HTOA: the database has none.
            AccurateRipChecksum ar = AccurateRipChecksum::forTrack(t, htoa ? plan.parts[1] : part);
            ripped.result = ripper.ripTrack(
                part,
                [&](const uint8_t* pcm, size_t bytes) {
                    if (cancelled_) throw RipCancelled();
                    writer->write(pcm, bytes);
                    if (!htoa) ar.update(pcm, bytes);
                    crc.update(pcm, bytes);
                },
                [&](uint32_t done, uint32_t total) {
                    if (cancelled_) throw RipCancelled();
                    const uint32_t inPart =
                        total ? uint32_t(uint64_t(done) * part.lengthSectors / total) : part.lengthSectors;
                    if (progress) progress(part.number, base + inPart, plan.totalSectors);
                });
            takeFallbacks(ripper, ripped.result);
            base += part.lengthSectors;
            ripped.accurateRipV1 = htoa ? 0 : ar.v1();
            ripped.accurateRipV2 = htoa ? 0 : ar.v2();
            parts.push_back(std::move(ripped));
        }
        writer->close();
        writer.reset();
        image.crc32 = crc.value();

        if (!cuePath.empty()) {
            std::ofstream out(cuePath, std::ios::binary | std::ios::trunc);
            out << plan.cueSheet;
            if (!out.flush()) throw std::runtime_error("failed to write " + plan.cueFileName);
        }
    } catch (...) {
        // No partial outputs (the writer, if any, is closed by now).
        std::error_code ec;
        std::filesystem::remove(imagePath, ec);
        if (!cuePath.empty()) std::filesystem::remove(cuePath, ec);
        throw;
    }

    ripped_.clear();
    for (RippedTrack& part : parts) {
        if (part.track.number == 0) image.htoa = std::move(part);
        else ripped_.push_back(std::move(part));
    }
    image_ = std::move(image);
    accurateRipChecked_ = false;
    return *image_;
}

const AccurateRipReport& RipSession::checkAccurateRip(HttpClient* http) {
    const Toc& t = toc();
    accurateRip_ = {};
    accurateRip_.id = AccurateRipDiscId::fromToc(t);
    accurateRipChecked_ = true;

    AccurateRipLookup lookup;
    if (http == nullptr) {
        accurateRip_.status = AccurateRipReport::Status::Disabled;
    } else {
        try {
            lookup = lookupAccurateRip(*http, accurateRip_.id);
        } catch (const std::exception& e) {  // lookupAccurateRip does not throw; be safe anyway
            lookup = {};
            lookup.error = e.what();
        }
        switch (lookup.status) {
            case AccurateRipLookup::Status::Found:
                accurateRip_.status = AccurateRipReport::Status::Found;
                break;
            case AccurateRipLookup::Status::NotFound:
                accurateRip_.status = AccurateRipReport::Status::NotFound;
                break;
            case AccurateRipLookup::Status::Error:
                accurateRip_.status = AccurateRipReport::Status::Error;
                accurateRip_.error = lookup.error;
                break;
        }
        accurateRip_.pressings = lookup.pressings.size();
    }
    for (const RippedTrack& r : ripped_)
        accurateRip_.tracks.push_back(matchAccurateRip(lookup.pressings, accurateRipEntryIndex(t, r.track),
                                                       r.track.number, r.accurateRipV1, r.accurateRipV2));
    return accurateRip_;
}

int RipSession::problemTracks() const {
    int n = image_ && image_->htoa && !image_->htoa->result.clean() ? 1 : 0;
    for (const RippedTrack& r : ripped_) n += r.result.clean() ? 0 : 1;
    return n;
}

std::string RipSession::ripLog() {
    const Toc& t = toc();
    const RipOptions& options = settings_.options;
    std::ostringstream log;
    log << "cdreader " << CDREADER_VERSION << " (Android) rip log\n"
        << "Drive: " << driveName() << "\n"
        << c2_.logLine() << "\n";
    for (const std::string& l : cacheCheck_.logLines()) log << l << "\n";
    log << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n"
        << readOffsetLogLine(options.readOffsetSamples, settings_.offsetSource) << "\n";
    // The detection behind an auto-detected offset, or the candidates the
    // user chose from (#37); otherwise the drive database's offset, if known.
    const ReadOffsetSource::Kind kind = settings_.offsetSource.kind;
    bool detectionShown = false;
    if (detection_ && kind == ReadOffsetSource::Kind::Detected && detection_->detected() &&
        detection_->offset == options.readOffsetSamples)
        detectionShown = true;
    if (detection_ && kind == ReadOffsetSource::Kind::Selected) {
        const std::vector<OffsetCandidate> all = detection_->allCandidates();
        detectionShown = std::any_of(all.begin(), all.end(), [&](const OffsetCandidate& c) {
            return c.offset == options.readOffsetSamples;
        });
    }
    if (detectionShown)
        for (const std::string& l : detection_->logLines()) log << l << "\n";
    if (driveOffsetDb_.status != DriveOffsetDbMatch::Status::NotChecked)
        log << driveOffsetDb_.logLine(options.readOffsetSamples) << "\n";
    log << "Format: " << settings_.format << "\n";
    if (const std::unique_ptr<AudioWriter> writer = createAudioWriter(settings_.format, settings_.encoder)) {
        const std::string encoder = writer->encoderDescription();
        if (!encoder.empty()) log << "Encoder: " << encoder << "\n";
    }
    log << "\n";

    char line[256];
    log << "CDDB disc id: " << hex32(t.cddbId()) << "\n";
    log << "AccurateRip disc id: " << AccurateRipDiscId::fromToc(t).toString() << "\n";
    for (const Track& track : t.tracks) {
        std::snprintf(line, sizeof line, "Track %2d  LBA %7u  %s  %-5s", track.number, track.startLba,
                      formatMsf(track.lengthSectors).c_str(), track.isAudio ? "audio" : "data");
        const std::string title = trackLabel(track.number);
        log << line << (title.empty() ? "" : "  ") << title << "\n";
    }
    log << "\n";
    for (const std::string& l : discCodes_.logLines()) log << l << "\n";
    log << "\n";
    for (const std::string& l : gaps_.logLines()) log << l << "\n";
    log << "\n";

    for (const std::string& l : cddbLookupLogLines(cddbLookedUp_ && cddbSettings_.enabled,
                                                  cddbSettings_.options.server, cddb_))
        log << l << "\n";
    log << "\n";

    log << "Folder: " << albumDirectoryName() << "\n";
    if (image_) {
        // The single-file layout of the CLI (#42).
        log << "Single file: " << image_->plan.fileName << "\n";
        if (image_->embeddedCueSheet)
            log << "Embedded CUE sheet: "
                << embeddedCueSheetDescription(fileExtension(image_->plan.fileName), image_->plan.withHtoa) << "\n";
        if (image_->htoa)
            for (const std::string& l : trackRipLogLines(image_->htoa->fileName, image_->htoa->result))
                log << l << "\n";
    }
    for (const RippedTrack& r : ripped_)
        for (const std::string& l : trackRipLogLines(r.fileName, r.result)) log << l << "\n";
    if (image_) log << discImageCrcLogLine(image_->plan.fileName, image_->crc32) << "\n";

    if (accurateRipChecked_) {
        log << "\n";
        for (const std::string& l : accurateRip_.logLines()) log << l << "\n";
    }

    log << "\n" << (problemTracks() ? "Finished with errors" : "All tracks ripped without errors") << "\n";
    return log.str();
}

}  // namespace cdr
