#include "rip_session.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>

#include "cdreader/audio_writer.h"
#include "cdreader/file_naming.h"

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "0.1.0"
#endif

namespace cdr {

namespace {

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

const std::string& RipSession::driveName() {
    if (!driveName_) driveName_ = drive_.inquiry().displayName();
    return *driveName_;
}

const Toc& RipSession::readToc() {
    toc_.reset();
    toc_ = drive_.readToc();
    cddb_ = {};
    cddbLookedUp_ = false;
    album_ = {};
    album_.discId = hex32(toc_->cddbId());
    discCodes_ = {};
    ripped_.clear();
    accurateRip_ = {};
    accurateRipChecked_ = false;
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
    ripped_.clear();
    accurateRip_ = {};
    accurateRipChecked_ = false;
    cancelled_ = false;
    if (settings.readDiscCodes && !discCodes_.read) readDiscCodes();
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
    ripped.accurateRipV1 = ar.v1();
    ripped.accurateRipV2 = ar.v2();

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
    int n = 0;
    for (const RippedTrack& r : ripped_) n += r.result.clean() ? 0 : 1;
    return n;
}

std::string RipSession::ripLog() {
    const Toc& t = toc();
    const RipOptions& options = settings_.options;
    std::ostringstream log;
    log << "cdreader " << CDREADER_VERSION << " (Android) rip log\n"
        << "Drive: " << driveName() << "\n"
        << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n"
        << "Read offset correction: " << (options.readOffsetSamples > 0 ? "+" : "") << options.readOffsetSamples
        << " samples\n"
        << "Format: " << settings_.format << "\n";
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

    if (!cddbLookedUp_ || !cddbSettings_.enabled) {
        log << "CDDB lookup: disabled\n\n";
    } else if (!cddb_.found) {
        log << "CDDB lookup (" << cddbSettings_.options.server << "): " << cddb_.error << "\n\n";
    } else {
        log << "CDDB lookup (" << cddbSettings_.options.server << "): " << cddb_.matches.size()
            << (cddb_.exact ? " exact" : " inexact") << " match(es)\n";
        for (size_t i = 0; i < cddb_.matches.size(); ++i) {
            const CddbMatch& m = cddb_.matches[i];
            log << (i == cddb_.chosen ? "  * " : "    ") << i + 1 << ". " << m.category << "/" << m.discId << "  "
                << m.title << "\n";
        }
        log << "Artist: " << album_.artist << "\nAlbum: " << album_.title << "\nYear: " << album_.year
            << "\nGenre: " << album_.genre << "\n\n";
    }

    log << "Folder: " << albumDirectoryName() << "\n";
    for (const RippedTrack& r : ripped_) {
        const std::string status =
            r.result.clean() ? "OK" : std::to_string(r.result.unreadableSectors) + " unreadable sector(s)";
        log << r.fileName << "  CRC32 " << hex32(r.result.crc32) << "  retries " << r.result.retries << "  " << status;
        if (r.result.paddedSamples)
            log << "  (" << r.result.paddedSamples << " samples outside the disc padded with silence)";
        log << "\n";
    }

    if (accurateRipChecked_) {
        log << "\n";
        for (const std::string& l : accurateRip_.logLines()) log << l << "\n";
    }

    log << "\n" << (problemTracks() ? "Finished with errors" : "All tracks ripped without errors") << "\n";
    return log.str();
}

}  // namespace cdr
