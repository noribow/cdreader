#pragma once

// After the tracks are read (#49): the AccurateRip report, the
// AccurateRip-guided re-read of tracks that had read problems and do not
// match the database, and the summary at the end of rip.log with the exit
// code of the CLI. Shared by the Windows CLI and the Android app.
//
// Why the re-read: a drive may "recover" a sector on a retry with wrong
// data and no error or C2 bits (seen on a real drive). The track then ends
// "OK" with retries, and only AccurateRip shows that it is wrong. Reading
// the track once more, slowly, with the drive cache defeated, often gets
// the right data; when AccurateRip confirms it, it replaces the first read.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/cd_drive.h"
#include "cdreader/clock.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"

namespace cdr {

// A database entry with at least this many submissions for the track makes
// a mismatch worth a re-read (one submission may be a bad rip itself).
inline constexpr int kAccurateRipRereadMinConfidence = 2;

// One AccurateRip-guided re-read of a track.
struct TrackReread {
    enum class Outcome {
        Accurate,       // the re-read matches AccurateRip: it replaces the first read
        SameData,       // the same data again (same CRC32), still not accurate
        DifferentData,  // other data, not accurate either: the first read is kept
        Failed,         // the re-read could not be made (see error): the first read is kept
        NotSupported,   // single-file image: not re-read
    };
    int track = 0;
    Outcome outcome = Outcome::Failed;
    int speedFactor = 0;     // the speed it was read at (x); 0: the speed of the rip (no SET CD SPEED)
    uint32_t firstCrc32 = 0;  // of the first read
    TrackRipResult result;   // of the re-read
    uint32_t v1 = 0;         // AccurateRip checksums of the re-read
    uint32_t v2 = 0;
    AccurateRipTrackResult accurateRip;  // the re-read compared with the database
    std::string error;       // Failed
    bool replaced = false;   // Accurate: the output file now holds the re-read

    bool accurate() const { return outcome == Outcome::Accurate; }
    static TrackReread notSupported(int track);

    // rip.log lines (indented to the pressing lines of the AccurateRip block):
    //   "Re-read at 4x: accurately ripped (v2 3), CRC32 4DD6A5D7 (first read 73B017B1), file replaced"
    //   "Re-read at 4x: same data (CRC32 73B017B1) - likely a different pressing or damage on the disc"
    //   "Re-read at 4x: different data (CRC32 1234ABCD, first read 73B017B1), not accurate either; the first read was kept"
    //   "Re-read at 4x failed: <error>; the first read was kept"
    //   "Re-read: not supported in single-file mode (rip the track on its own to re-read it)"
    // plus the re-read's own retries / C2 errors / slowdown when it had any.
    std::vector<std::string> logLines() const;
};

// Whether a track should be re-read: AccurateRip has an entry with at least
// `minConfidence` submissions for it, none matches the rip, and the read had
// problems (TrackRipResult::readProblems(): retries, C2 errors, slowdown).
bool wantsAccurateRipReread(const AccurateRipTrackResult& accurateRip, const TrackRipResult& result,
                            int minConfidence = kAccurateRipRereadMinConfidence);

// The speed (x) of the re-read: the slowest of options.slowdownSpeeds; 0 when
// slowdown is off (options.slowdown: disabled by the user or given up because
// the drive rejects SET CD SPEED) or has no speeds. The re-read is tied to
// slowdown: no slowdown, no re-read.
int accurateRipRereadSpeed(const RipOptions& options);

// The rip.log line of the setting: "AccurateRip re-read: at 4x (tracks with
// read problems that do not match)" / "AccurateRip re-read: disabled" /
// "AccurateRip re-read: not possible without slowdown".
std::string accurateRipRereadLogLine(bool enabled, const RipOptions& options);

// Reads `track` again at accurateRipRereadSpeed(options) (SET CD SPEED,
// unless the drive already reads at that speed or slower; afterwards SET CD
// SPEED options.speedKBps restores the speed of the rip), with the drive
// cache defeated before the first block, and with the retries / C2 / cache
// defeat of `options`. The offset-corrected PCM goes to `sink` (the caller
// writes it to a new file); the checksums are compared with `pressings`.
// `firstCrc32`: the CRC32 of the first read, for the outcome. A drive that
// rejects SET CD SPEED gives Failed. Exceptions thrown by `sink` / `progress`
// propagate (after the speed was restored).
TrackReread rereadForAccurateRip(CdDrive& drive, const Toc& toc, const RipOptions& options, const Track& track,
                                 const std::vector<AccurateRipPressing>& pressings, uint32_t firstCrc32,
                                 const Ripper::SampleSink& sink, const Ripper::Progress& progress = {},
                                 Clock* clock = nullptr);

// The AccurateRip result of a rip, with the re-reads.
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
    size_t pressings = 0;                        // records in the database file
    std::vector<AccurateRipTrackResult> tracks;  // one per ripped track (first read), in rip order
    std::map<int, TrackReread> rereads;          // by track number

    // From a lookup (`lookedUp` false: Disabled).
    void setLookup(const AccurateRipLookup& lookup, bool lookedUp);

    // The re-read of a track, or null.
    const TrackReread* reread(int track) const;
    // Accurate with the first read or the re-read.
    bool accurate(const AccurateRipTrackResult& track) const;
    int accurateTracks() const;
    int tracksInDatabase() const;

    // The AccurateRip block of rip.log: the disc id, per track its
    // checksums, the verdict, the pressings and the re-read, then the
    // totals. `disabledLine`: the line when the lookup was off; `hint`: the
    // line when the disc is in the database but no track matched.
    std::vector<std::string> logLines(const std::string& disabledLine = "AccurateRip: lookup disabled",
                                      const std::string& hint = "Hint: no track matched. Check the read offset.") const;
};

// The end of a rip (#49): which tracks had problems, what AccurateRip says
// about them, and the verdict.
struct RipSummary {
    enum class Verdict {
        Clean,      // no read problems, no track that AccurateRip says is not accurate
        Resolved,   // read problems, but AccurateRip confirms every track that had them
        Warnings,   // read problems AccurateRip cannot confirm, or tracks not accurate without read problems
        Errors,     // unreadable sectors / suspicious positions, or not accurate after read problems
    };
    Verdict verdict = Verdict::Clean;
    std::vector<int> errorTracks;      // unreadable sectors or suspicious positions
    std::map<int, std::string> problemTracks;  // TrackRipResult::readProblemText() by track
    std::vector<int> notAccurate;      // in the database, not accurate (after the re-read)
    std::vector<int> notAccurateWithProblems;  // ... and with read problems
    std::vector<int> rereadTracks;     // re-read after AccurateRip (not NotSupported)
    std::vector<int> rereadAccurate;   // ... and accurate then

    // CLI exit code: 2 for Errors, otherwise 0.
    int exitCode() const { return verdict == Verdict::Errors ? 2 : 0; }
    // "All tracks ripped without errors" / "All tracks ripped; the read
    // problems were resolved (AccurateRip confirms the tracks)" / "Finished
    // with warnings" / "Finished with errors".
    std::string finalLine() const;
    // The summary lines before finalLine() (empty when clean):
    //   "Tracks with unreadable sectors or suspicious positions: 3"
    //   "Tracks with read problems: 2, 4 (retries)" (per track when they differ)
    //   "Re-read after AccurateRip: 2 track(s), 1 accurately ripped (track 2)"
    //   "2 track(s) not accurate in AccurateRip (tracks 2, 4; read errors were retried)"
    std::vector<std::string> lines() const;
};

// One ripped track or image part (track 0: the HTOA) for the summary.
struct RippedTrackResult {
    int track = 0;
    const TrackRipResult* result = nullptr;
};

// `report`: null when AccurateRip was not consulted.
RipSummary summarizeRip(const std::vector<RippedTrackResult>& tracks, const AccurateRipReport* report);

}  // namespace cdr
