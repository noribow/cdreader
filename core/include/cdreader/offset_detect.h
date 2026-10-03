#pragma once

// Read offset auto-detection (#37) with the AccurateRip database, shared by
// the CLI (`cdreader offset`, `rip --offset auto`) and the Android app, and
// the per-drive store of detected offsets.
//
// Detection reads a few tracks of the inserted disc at offset 0, extended by
// `maxOffset` samples on both sides (scanReadOffsets()), and looks for the
// offsets in [-maxOffset, +maxOffset] whose v1 or v2 checksum equals a
// database entry (findAccurateRipOffsets()). An offset is only confirmed
// when several tracks agree on it (see OffsetDetectOptions::minAgreeingTracks):
// a single coincidental match must not set a wrong offset for every later
// rip with this drive.
//
// Pressings shifted against each other: a popular disc often has several
// pressings in the database whose audio is the same, only shifted by a fixed
// number of samples. Then every track matches at several offsets, one per
// pressing (e.g. +6, -145, -658 for a drive whose offset is +6). When every
// track read shows the same set of offsets (an offset may only be missing
// on a track whose database entries lack the pressings it matched), this is
// not a conflict: each offset is scored by the submissions behind it summed
// over the tracks, and the best one is confirmed when its score is at least
// OffsetDetectOptions::pressingScoreRatio times the runner-up's and it
// matched on at least two tracks (the drive's own offset is the one most
// people ripped with). Otherwise the result is Ambiguous. Tracks matching at
// disjoint offsets remain a Conflict.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/cd_drive.h"
#include "cdreader/http.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"

namespace cdr {

struct OffsetDetectProgress {
    int step = 0;           // 1-based number of the track being read
    int steps = 0;          // tracks that will be read at most
    int track = 0;          // its track number
    uint32_t doneSectors = 0;
    uint32_t totalSectors = 0;
};

struct OffsetDetectOptions {
    // Offsets tried: -maxOffset..+maxOffset samples (known drives lie within
    // about +-3000; at most 100 sectors' worth).
    uint32_t maxOffset = 3000;
    // Tracks read at most (the scan stops early once the offset is confirmed).
    int maxTracks = 3;
    // Tracks that must match at the same offset. A disc with fewer usable
    // tracks than this (e.g. a single) needs only its one track, but then
    // that track's matching submissions must reach singleTrackMinConfidence.
    int minAgreeingTracks = 2;
    int singleTrackMinConfidence = 10;
    // Pressings shifted against each other: the best offset needs at least
    // this many times the summed confidence of the next one. 1.5 separates
    // the main pressing plus its v1 submissions (e.g. 14 + 7 per track)
    // from the next pressing (13), but not two pressings of about the same
    // size, where the drive's offset cannot be told from the database.
    double pressingScoreRatio = 1.5;
    // Preferred track lengths: shorter tracks carry little data (and often
    // few submissions), longer ones take long to read and need about
    // 4 bytes per sample of memory. Tracks outside the range are only read
    // when there are not enough others.
    uint32_t minTrackSectors = 10 * kSectorsPerSecond;
    uint32_t maxTrackSectors = 8 * 60 * kSectorsPerSecond;
    // Read these tracks instead of choosing (CLI `offset -t`), in this order.
    std::vector<int> tracks;
    // Reads: retries, verify (the read offset is overridden).
    RipOptions rip;
    // Called while reading; may throw to abort.
    std::function<void(const OffsetDetectProgress&)> progress;
    // Polled during the reads: true aborts with status Cancelled.
    std::function<bool()> cancelled;
};

// One track that was read.
struct OffsetDetectTrack {
    int track = 0;
    int totalConfidence = 0;  // all submissions for the track
    std::vector<int> pressingsWithEntry;  // 1-based database records with an entry for the track
    std::vector<AccurateRipOffsetMatch> matches;  // highest confidence first
};

// An offset matched by pressings shifted against each other, with the
// submissions behind it summed over the tracks read (each pressing once
// per track; v1 and v2 hits of different pressings both count).
struct OffsetCandidate {
    int offset = 0;
    int tracks = 0;  // tracks that match at it
    int v1Confidence = 0;
    int v2Confidence = 0;
    std::vector<int> pressings;  // 1-based database records that match at it (any track)

    int score() const { return v1Confidence + v2Confidence; }
    std::string matchedVersion() const;  // "v1", "v2", "v1+v2"
};

struct OffsetDetection {
    enum class Status {
        Detected,        // offset is confirmed
        NotInDatabase,   // the disc is not in AccurateRip: cannot detect
        LookupFailed,    // network / protocol error (see error)
        NoUsableTracks,  // no audio track has database entries
        NoMatch,         // no offset in range matches any track
        NotEnough,       // matches, but too few tracks agree (or one track with low confidence)
        Conflict,        // tracks match at different offsets
        Cancelled,
        Ambiguous,       // every track matches the same offsets (shifted pressings), none clearly best
    };
    Status status = Status::NoMatch;
    std::string error;            // LookupFailed / read errors
    AccurateRipDiscId id;
    uint32_t maxOffset = 0;
    size_t pressings = 0;         // records in the database file
    int usableTracks = 0;         // audio tracks with database entries
    std::vector<OffsetDetectTrack> tracks;  // read so far, in reading order

    // Detected (and the best candidate otherwise):
    int offset = 0;
    int agreeingTracks = 0;       // tracks that match at `offset`
    int v1Confidence = 0;         // their submissions at `offset`
    int v2Confidence = 0;
    std::vector<int> offsetPressings;  // 1-based database records that match at `offset`
    bool singleTrack = false;     // confirmed by the disc's only usable track
    std::vector<int> candidates;  // Conflict / Ambiguous: the competing offsets (Ambiguous: best score first)
    // Detected / Ambiguous with shifted pressings: the other pressings'
    // offsets, highest score first (the chosen / best one is `offset`).
    std::vector<OffsetCandidate> alternatives;

    bool detected() const { return status == Status::Detected; }
    int testedTracks() const { return int(tracks.size()); }
    int confidence() const { return v1Confidence + v2Confidence; }
    std::string matchedVersion() const;  // "v1", "v2", "v1+v2"

    // "2 of 3 tracks agreed, v2" (Detected), used in rip.log and the store;
    // with shifted pressings "2 of 2 tracks agreed, v1+v2; also -145 (26),
    // -658 (8): other pressings".
    std::string agreement() const;
    // "also -145 (26), -658 (8): other pressings" (score in parentheses), or empty.
    std::string alternativesText() const;
    // One English sentence: the result, or why there is none.
    std::string summary() const;
    // Block for rip.log / the console: the result and every track read.
    std::vector<std::string> logLines() const;
};

// The tracks detection would read, in order of preference: inside the disc
// (not the first / last audio track, which touch the unreadable area outside
// it at large offsets), within the preferred lengths, highest confidence.
std::vector<const Track*> chooseOffsetTracks(const Toc& toc, const std::vector<AccurateRipPressing>& pressings,
                                             const OffsetDetectOptions& options);

// Detection against database records already downloaded. Never throws for
// read errors of a track (they end up in `error`, status NoMatch / NotEnough
// unless other tracks decide), but exceptions from options.progress propagate.
OffsetDetection detectReadOffset(CdDrive& drive, const Toc& toc, const std::vector<AccurateRipPressing>& pressings,
                                 const OffsetDetectOptions& options = {});

// Looks the disc up first (statuses NotInDatabase / LookupFailed).
OffsetDetection detectReadOffset(CdDrive& drive, const Toc& toc, HttpClient& http,
                                 const OffsetDetectOptions& options = {});

// --- Where the read offset of a rip came from (rip.log) ------------------------
struct ReadOffsetSource {
    enum class Kind { Manual, Saved, Detected };
    Kind kind = Kind::Manual;
    // Saved: the drive ("HL-DT-ST BD-RE BP71N (1.03)") and the stored note;
    // Detected: OffsetDetection::agreement().
    std::string detail;

    // "manual", "saved for drive HL-DT-ST BD-RE BP71N (1.03); auto-detected:
    // 2 of 2 tracks agreed, v2", "auto-detected: 3 of 3 tracks agreed, v1+v2".
    std::string describe() const;
};

// "Read offset correction: +6 samples (auto-detected: 2 of 2 tracks agreed, v2)".
std::string readOffsetLogLine(int offset, const ReadOffsetSource& source);

// --- Detected offsets per drive ---------------------------------------------------
// Key of a drive model: "VENDOR|PRODUCT|REVISION" (INQUIRY strings, trimmed;
// '|', tabs and line breaks replaced by spaces). Revision is part of it
// because firmware updates have changed offsets in the past.
std::string driveOffsetKey(const DriveInfo& info);

struct SavedDriveOffset {
    int offset = 0;
    std::string note;  // e.g. "auto-detected: 2 of 2 tracks agreed, v2"
};

// Text format, one drive per line (UTF-8, '#' starts a comment):
//   <offset> TAB <key> TAB <note>
// Unparsable lines are ignored, so that a damaged file loses one entry at most.
class DriveOffsetStore {
public:
    static DriveOffsetStore parse(const std::string& text);
    std::string serialize() const;

    const SavedDriveOffset* find(const std::string& key) const;
    void set(const std::string& key, const SavedDriveOffset& value);
    bool erase(const std::string& key);
    const std::map<std::string, SavedDriveOffset>& entries() const { return entries_; }

private:
    std::map<std::string, SavedDriveOffset> entries_;
};

}  // namespace cdr
