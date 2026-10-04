#pragma once

// Ripping workflow of the Android app, kept free of JNI so that it can be
// unit tested on any platform: CDDB lookup, per-track ripping to WAV / FLAC / Ogg FLAC / ALAC / Opus / Vorbis
// with tags and CDDB based file names, or the whole disc into one file with a
// CUE sheet (#42), AccurateRip checksums and lookup, and the rip.log text.
// jni_bridge.cpp only converts arguments and results.
//
// All calls must come from one thread at a time, except cancel().

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/clock.h"
#include "cdreader/disc_image.h"
#include "cdreader/drive_cache.h"
#include "cdreader/drive_offset_db.h"
#include "cdreader/gaps.h"
#include "cdreader/http.h"
#include "cdreader/metadata.h"
#include "cdreader/offset_detect.h"
#include "cdreader/ripper.h"
#include "cdreader/subchannel.h"
#include "cdreader/toc.h"

namespace cdr {

// Thrown by RipSession::ripTrack() after cancel().
class RipCancelled : public std::runtime_error {
public:
    RipCancelled() : std::runtime_error("rip cancelled") {}
};

struct CddbSettings {
    bool enabled = true;
    CddbOptions options;  // server, match index, client info
};

// Settings shared by all tracks of one rip (see RipSession::beginRip()).
struct RipSettings {
    std::string format = "wav";  // a name accepted by createAudioWriter()
    EncoderSettings encoder;     // lossy formats (default: the format's defaults)
    RipOptions options;
    // Read the MCN and the ISRCs from the Q sub-channel (once per disc) for
    // the tags and rip.log.
    bool readDiscCodes = true;
    // Detect pregaps / index points / the HTOA from the Q sub-channel (once
    // per disc, #25): rip.log, and the CUE sheet of ripImage() (#42).
    bool detectGaps = true;
    // Read with C2 error pointers when the drive supports them (#33);
    // beginRip() checks the drive and sets options.useC2 accordingly.
    bool useC2 = true;
    // Drive cache defeat for re-reads (#34); beginRip() decides the method
    // (Auto: a timing test, once per disc) and sets options.cacheDefeat.
    CacheSetting cache = CacheSetting::Auto;
    // Where options.readOffsetSamples came from (#37), for rip.log.
    ReadOffsetSource offsetSource;
};

struct RippedTrack {
    Track track;
    std::string fileName;  // e.g. "01 - Title.flac"; in a disc image the part's name ("Track 01")
    TrackRipResult result;
    uint32_t accurateRipV1 = 0;
    uint32_t accurateRipV2 = 0;
};

// A single-file rip (#42): the tracks in one file, as `cdreader rip --single-file`.
struct RippedImage {
    DiscImagePlan plan;          // file names, parts (HTOA first when included) and CUE sheet
    // The writer carries the CUE sheet inside the file (FLAC / Ogg FLAC:
    // CUESHEET block and tag; Matroska: chapters); otherwise only the .cue file has it.
    bool embeddedCueSheet = false;
    std::optional<RippedTrack> htoa;  // the HTOA part (no AccurateRip checksums: the database has none)
    uint32_t crc32 = 0;               // CRC32 of all the PCM in the image
};

struct AccurateRipReport {
    enum class Status {
        Disabled,  // the lookup was turned off
        Found,
        NotFound,  // no rip of this disc has been submitted
        Error,     // network or protocol failure (see error)
    };
    Status status = Status::Disabled;
    std::string error;
    AccurateRipDiscId id;
    size_t pressings = 0;  // records in the database file
    std::vector<AccurateRipTrackResult> tracks;  // one per ripped track, in rip order

    int accurateTracks() const;
    int tracksInDatabase() const;

    // The AccurateRip block of rip.log, in the same format as the Windows CLI.
    std::vector<std::string> logLines() const;
};

class RipSession {
public:
    explicit RipSession(CdDrive& drive) : drive_(drive) {}

    // The clock of the cache detection (default: steadyClock()); tests use
    // the fake drive's simulated time. `clock` must outlive the session.
    void setClock(Clock& clock) { clock_ = &clock; }

    // INQUIRY strings / display name (cached after the first call).
    const DriveInfo& driveInfo();
    const std::string& driveName();
    // Key of the drive model for the saved read offsets (#37).
    std::string driveOffsetKey() { return cdr::driveOffsetKey(driveInfo()); }

    // Reads the TOC (again) and forgets the metadata, the cache detection and
    // the rip results of the previous disc.
    const Toc& readToc();
    // The TOC read last, read now if there is none yet.
    const Toc& toc();

    // Looks the disc up (or clears the metadata when disabled). Never throws
    // for network problems; `http` may be null when the lookup is disabled.
    const CddbLookupResult& lookupCddb(HttpClient* http, const CddbSettings& settings);
    const CddbLookupResult& cddbResult() const { return cddb_; }
    const CddbSettings& cddbSettings() const { return cddbSettings_; }

    // Reads the MCN and the ISRCs of the audio tracks (again). Never throws
    // for a drive that cannot read them; beginRip() calls it when enabled.
    const DiscCodes& readDiscCodes();
    const DiscCodes& discCodes() const { return discCodes_; }

    // Detects the pregaps, index points and HTOA (again). Never throws for a
    // drive that cannot read the Q sub-channel; beginRip() calls it when enabled.
    const DiscGaps& detectGaps();
    // Before detection: the HTOA from the TOC only (status NotRun).
    const DiscGaps& discGaps() const { return gaps_; }

    // Album metadata from CDDB (empty without a match); discId is always set,
    // and the MCN / ISRCs once read.
    const AlbumMetadata& album() const { return album_; }
    TrackMetadata trackMetadata(int number);
    // "Title" or "Artist / Title" of a track, empty if unknown.
    std::string trackLabel(int number);
    // "Artist - Album" or "cd_<disc id>".
    std::string albumDirectoryName() const;
    // "NN - Title.<ext>" or "TrackNN.<ext>". Throws std::invalid_argument for an unknown format.
    std::string trackFileName(int number, const std::string& format);

    // Starts a rip: validates the settings, forgets earlier rip results,
    // clears a pending cancel, checks C2 support and the drive cache (one
    // MODE SENSE when either is wanted; the cache timing test of the Auto
    // setting only once per disc) and reads the MCN / ISRCs and detects the
    // gaps if not done yet for this disc (settings.readDiscCodes /
    // detectGaps). Throws std::invalid_argument.
    void beginRip(const RipSettings& settings);
    // As used: options.useC2 is what the drive allows (and false after a
    // fallback to plain reads during the rip), options.cacheDefeat the method
    // of the cache check (Flush after the drive rejected FUA during the rip).
    const RipSettings& ripSettings() const { return settings_; }
    // C2 support found by beginRip() (Disabled before it or when !useC2).
    const C2Availability& c2Availability() const { return c2_; }
    // Why C2 reads were given up during this rip (empty: they were not).
    const std::string& c2Fallback() const { return c2Fallback_; }
    // The drive cache check of beginRip() (setting None before it).
    const DriveCacheCheck& cacheCheck() const { return cacheCheck_; }
    // Why FUA was given up during this rip (empty: it was not).
    const std::string& cacheFallback() const { return cacheFallback_; }

    // Rips one audio track to `path` (a seekable local file: WAV, FLAC and Ogg
    // FLAC headers are patched when the file is closed), computing its AccurateRip
    // checksums from the same PCM stream. Throws RipCancelled after cancel()
    // and std::runtime_error / ScsiError on errors; a partial file is left
    // for the caller to delete. Exceptions thrown by `progress` propagate.
    const RippedTrack& ripTrack(int number, const std::filesystem::path& path, const Ripper::Progress& progress = {});
    const std::vector<RippedTrack>& rippedTracks() const { return ripped_; }

    // Single-file mode (#42). The image of `tracks` (track numbers; empty:
    // every audio track) in `format`: "<Artist> - <Album>.<ext>" or
    // "CDImage.<ext>", the CUE sheet text and the parts, from the current
    // metadata and gaps. Throws std::invalid_argument for an unknown format,
    // a track that is not an audio track on this disc or tracks that are not
    // consecutive (the file would be no image of the disc).
    DiscImagePlan planImage(const std::vector<int>& tracks, const std::string& format);
    // Progress of ripImage(): the part being read (0: the HTOA) and the
    // sectors done of the whole image.
    using ImageProgress = std::function<void(int track, uint32_t doneSectors, uint32_t totalSectors)>;
    // Rips `tracks` (as planImage(), in the format of beginRip()) into one
    // file at `imagePath` (a seekable local file, as ripTrack()), the HTOA
    // first when the disc has one and track 1 is included, with the CUE
    // sheet embedded where the format can carry it, and writes the CUE sheet
    // to `cuePath` (empty: not written). AccurateRip checksums are computed
    // per track from the same PCM stream; the tracks' results replace those
    // of earlier rips (rippedTracks(), with the part names as file names) and
    // rip.log gets the single-file layout of the CLI. On cancel() (throws
    // RipCancelled) or any error both files are deleted. Exceptions thrown
    // by `progress` propagate (the files are deleted as well).
    const RippedImage& ripImage(const std::vector<int>& tracks, const std::filesystem::path& imagePath,
                                const std::filesystem::path& cuePath, const ImageProgress& progress = {});
    // The image of the current rip (beginRip() and ripTrack() clear it).
    const std::optional<RippedImage>& rippedImage() const { return image_; }

    // AccurateRip drive offset database (#37): where DriveOffsets.bin is
    // stored between runs (the app's cache folder; empty: not stored).
    void setDriveOffsetDbCache(std::filesystem::path file) { driveOffsetDbCache_ = std::move(file); }
    // The drive's entry: the stored file when fresh (kDriveOffsetDbMaxAgeSeconds),
    // otherwise downloaded with `http` (null: only the stored file) and
    // stored; a stale stored file when the download fails. Never throws.
    // The result stays for the session (it belongs to the drive, not the disc).
    const DriveOffsetDbMatch& lookupDriveOffsetDb(HttpClient* http);
    const DriveOffsetDbMatch& driveOffsetDb() const { return driveOffsetDb_; }

    // Read offset auto-detection (#37): consults the drive offset database
    // (lookupDriveOffsetDb()), looks the disc up in AccurateRip
    // (`http` null: status LookupFailed) and reads a few tracks. Clears a
    // pending cancel first; cancel() makes it return status Cancelled.
    // Never throws for network or read problems; exceptions thrown by
    // options.progress propagate. The result stays until the next readToc().
    const OffsetDetection& detectReadOffset(HttpClient* http, OffsetDetectOptions options = {});
    const std::optional<OffsetDetection>& offsetDetection() const { return detection_; }

    // Thread-safe: makes a running ripTrack() / detectReadOffset() stop at the next block.
    void cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }

    // Looks the ripped tracks up in the AccurateRip database (`http` null:
    // disabled). Never throws for network problems.
    const AccurateRipReport& checkAccurateRip(HttpClient* http);
    const AccurateRipReport& accurateRipReport() const { return accurateRip_; }

    // Tracks with unreadable sectors or suspicious positions (C2).
    int problemTracks() const;

    // rip.log for the current rip (drive, settings, TOC, CDDB, tracks,
    // AccurateRip), in the format of the Windows CLI.
    std::string ripLog();

private:
    // After a track or image part: the drive rejected C2 reads or FUA during
    // it, so the rest of the rip does without.
    void takeFallbacks(const Ripper& ripper, const TrackRipResult& result);

    CdDrive& drive_;
    std::optional<DriveInfo> driveInfo_;
    std::optional<std::string> driveName_;
    std::optional<Toc> toc_;
    CddbSettings cddbSettings_;
    CddbLookupResult cddb_;
    bool cddbLookedUp_ = false;
    AlbumMetadata album_;
    DiscCodes discCodes_;
    DiscGaps gaps_;
    RipSettings settings_;
    C2Availability c2_;
    std::string c2Fallback_;
    Clock* clock_ = &steadyClock();
    DriveCacheCheck cacheCheck_;
    std::optional<DriveCacheCheck> detectedCache_;  // Auto: the timing test of this disc
    std::string cacheFallback_;
    std::vector<RippedTrack> ripped_;
    std::optional<RippedImage> image_;
    AccurateRipReport accurateRip_;
    bool accurateRipChecked_ = false;
    std::optional<OffsetDetection> detection_;
    std::filesystem::path driveOffsetDbCache_;
    DriveOffsetDbMatch driveOffsetDb_;
    std::atomic<bool> cancelled_{false};
};

}  // namespace cdr
