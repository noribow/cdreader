#pragma once

// Ripping workflow of the Android app, kept free of JNI so that it can be
// unit tested on any platform: CDDB lookup, per-track ripping to WAV / FLAC / Ogg FLAC / ALAC / Opus / Vorbis
// with tags and CDDB based file names, AccurateRip checksums and lookup, and
// the rip.log text. jni_bridge.cpp only converts arguments and results.
//
// All calls must come from one thread at a time, except cancel().

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/gaps.h"
#include "cdreader/http.h"
#include "cdreader/metadata.h"
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
    // per disc, #25). The app writes no CUE sheet yet: they go to rip.log.
    bool detectGaps = true;
    // Read with C2 error pointers when the drive supports them (#33);
    // beginRip() checks the drive and sets options.useC2 accordingly.
    bool useC2 = true;
};

struct RippedTrack {
    Track track;
    std::string fileName;  // e.g. "01 - Title.flac"
    TrackRipResult result;
    uint32_t accurateRipV1 = 0;
    uint32_t accurateRipV2 = 0;
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

    // INQUIRY display name (cached after the first call).
    const std::string& driveName();

    // Reads the TOC (again) and forgets the metadata and rip results of the
    // previous disc.
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
    // clears a pending cancel and reads the MCN / ISRCs and detects the gaps
    // if not done yet for this disc (settings.readDiscCodes / detectGaps).
    // Throws std::invalid_argument.
    void beginRip(const RipSettings& settings);
    // As used: options.useC2 is what the drive allows (and false after a
    // fallback to plain reads during the rip).
    const RipSettings& ripSettings() const { return settings_; }
    // C2 support found by beginRip() (Disabled before it or when !useC2).
    const C2Availability& c2Availability() const { return c2_; }
    // Why C2 reads were given up during this rip (empty: they were not).
    const std::string& c2Fallback() const { return c2Fallback_; }

    // Rips one audio track to `path` (a seekable local file: WAV, FLAC and Ogg
    // FLAC headers are patched when the file is closed), computing its AccurateRip
    // checksums from the same PCM stream. Throws RipCancelled after cancel()
    // and std::runtime_error / ScsiError on errors; a partial file is left
    // for the caller to delete. Exceptions thrown by `progress` propagate.
    const RippedTrack& ripTrack(int number, const std::filesystem::path& path, const Ripper::Progress& progress = {});
    const std::vector<RippedTrack>& rippedTracks() const { return ripped_; }

    // Thread-safe: makes a running ripTrack() stop at the next block.
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
    CdDrive& drive_;
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
    std::vector<RippedTrack> ripped_;
    AccurateRipReport accurateRip_;
    bool accurateRipChecked_ = false;
    std::atomic<bool> cancelled_{false};
};

}  // namespace cdr
