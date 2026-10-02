#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/http.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"

namespace cdr {

// AccurateRip (www.accuraterip.com): a database of per-track checksums
// submitted by other rippers. A rip whose checksum matches entries from
// other people is very likely bit-exact. The algorithms follow the reference
// implementations (dBpoweramp / EAC, as reproduced by ARver and whipper).

// Samples ignored at the start of the first and the end of the last audio
// track (5 sectors), because drives with different read offsets cannot all
// read them.
constexpr uint32_t kAccurateRipSkipSamples = 5 * kSamplesPerSector;

// The disc fingerprint used to locate the database file.
struct AccurateRipDiscId {
    int audioTracks = 0;  // number of audio tracks (data tracks excluded)
    uint32_t id1 = 0;     // sum of the audio track offsets and the lead-out
    uint32_t id2 = 0;     // the same, weighted by the audio track index
    uint32_t cddb = 0;    // freedb disc id over all tracks, data included

    // Data tracks are skipped for id1/id2, but the lead-out of the whole disc
    // is used, so for an Enhanced CD it lies after the data session.
    static AccurateRipDiscId fromToc(const Toc& toc);

    std::string toString() const;  // "nnn-xxxxxxxx-xxxxxxxx-xxxxxxxx" (lowercase hex)
    std::string url() const;       // http://www.accuraterip.com/accuraterip/a/b/c/dBAR-....bin

    bool operator==(const AccurateRipDiscId& o) const {
        return audioTracks == o.audioTracks && id1 == o.id1 && id2 == o.id2 && cddb == o.cddb;
    }
    bool operator!=(const AccurateRipDiscId& o) const { return !(*this == o); }
};

// Streaming AccurateRip v1 / v2 checksum of one track's offset-corrected PCM
// (16-bit stereo little-endian; one 32-bit sample = left | right << 16).
class AccurateRipChecksum {
public:
    // `trackSamples`: the track length in samples; `firstTrack` / `lastTrack`:
    // whether this is the first / last audio track of the disc.
    AccurateRipChecksum(uint32_t trackSamples, bool firstTrack, bool lastTrack);

    // Checksum for `track` of `toc` (position among the audio tracks decides
    // which samples are skipped).
    static AccurateRipChecksum forTrack(const Toc& toc, const Track& track);

    // Accepts any chunking, including chunks that split a sample.
    void update(const uint8_t* pcm, size_t bytes);

    uint32_t v1() const { return lo_; }
    uint32_t v2() const { return lo_ + hi_; }
    uint32_t samples() const { return position_; }

private:
    void add(uint32_t sample);

    uint32_t first_;  // 1-based positions included in the sum: [first_, last_]
    uint32_t last_;
    uint32_t position_ = 0;
    uint32_t lo_ = 0;
    uint32_t hi_ = 0;
    uint8_t partial_[4] = {};
    size_t partialBytes_ = 0;
};

// One track entry of a database record.
struct AccurateRipEntry {
    uint8_t confidence = 0;      // number of submissions with this checksum
    uint32_t checksum = 0;       // v1 or v2 (the database does not say which)
    uint32_t frame450Checksum = 0;  // used by offset detection
};

// One record ("pressing") of a dBAR file. A file holds one or more records,
// each with an entry per track in TOC order (for a Mixed Mode CD the data
// track occupies the first slot).
struct AccurateRipPressing {
    AccurateRipDiscId id;
    std::vector<AccurateRipEntry> tracks;
};

// Parses a binary dBAR response. Throws std::runtime_error on truncated data.
std::vector<AccurateRipPressing> parseAccurateRipResponse(const std::string& body);

struct AccurateRipLookup {
    enum class Status {
        Found,     // the disc is in the database
        NotFound,  // HTTP 404: no rip of this disc has been submitted
        Error,     // network failure, unexpected status or malformed response
    };
    Status status = Status::Error;
    std::string error;
    std::vector<AccurateRipPressing> pressings;
};

// Downloads and parses the database record for `id`. Never throws.
AccurateRipLookup lookupAccurateRip(HttpClient& http, const AccurateRipDiscId& id);

struct AccurateRipTrackResult {
    int track = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    int v1Confidence = 0;     // submissions whose checksum equals v1
    int v2Confidence = 0;     // submissions whose checksum equals v2
    int totalConfidence = 0;  // all submissions for this track

    bool inDatabase() const { return totalConfidence > 0; }
    bool accurate() const { return v1Confidence > 0 || v2Confidence > 0; }
    int confidence() const { return v1Confidence + v2Confidence; }

    // e.g. "Accurately ripped (confidence 12/15, v2)", "Not accurate (0/15)",
    // "Not in database".
    std::string describe() const;
};

// Compares a track's checksums with every pressing. `entryIndex` is the
// track's 0-based position in the TOC (see accurateRipEntryIndex()).
AccurateRipTrackResult matchAccurateRip(const std::vector<AccurateRipPressing>& pressings, size_t entryIndex,
                                        int trackNumber, uint32_t v1, uint32_t v2);

// Position of `track` within the database records: its index in the TOC.
size_t accurateRipEntryIndex(const Toc& toc, const Track& track);

// --- Read offset detection --------------------------------------------------
// Computes the v1 checksum of a track for every read offset in
// [-maxOffset, +maxOffset] in one pass. Feed it the track read with offset
// correction -maxOffset and extended by 2 * maxOffset samples (missing data
// counts as silence). A database entry that matches the checksum for offset
// X means that "--offset X" reproduces the submitted rips.
class AccurateRipOffsetScan {
public:
    AccurateRipOffsetScan(uint32_t trackSamples, bool firstTrack, bool lastTrack, uint32_t maxOffset);
    static AccurateRipOffsetScan forTrack(const Toc& toc, const Track& track, uint32_t maxOffset);

    void update(const uint8_t* pcm, size_t bytes);

    // v1 checksums; element i belongs to offset i - maxOffset.
    std::vector<uint32_t> checksums() const;

    uint32_t maxOffset() const { return maxOffset_; }
    uint32_t samplesNeeded() const { return trackSamples_ + 2 * maxOffset_; }

private:
    void add(uint32_t sample);

    uint32_t trackSamples_;
    uint32_t maxOffset_;
    uint32_t first_;  // 1-based window of the checksum, as in AccurateRipChecksum
    uint32_t last_;
    uint64_t index_ = 0;  // samples received
    uint32_t v1_ = 0;     // checksum and plain sum of the window at offset -maxOffset
    uint32_t sum_ = 0;
    std::vector<uint32_t> leaving_;   // samples that leave the window as the offset grows
    std::vector<uint32_t> entering_;  // samples that enter it
    uint8_t partial_[4] = {};
    size_t partialBytes_ = 0;
};

// Reads `track` for an offset scan (with `options` for retries / verify; the
// read offset is overridden) and returns the filled scan.
AccurateRipOffsetScan scanReadOffsets(CdDrive& drive, const Toc& toc, const Track& track, uint32_t maxOffset,
                                      RipOptions options, const Ripper::Progress& progress = {});

struct AccurateRipOffsetMatch {
    int offset = 0;
    int confidence = 0;  // submissions whose checksum matches at this offset
};

// Offsets whose checksum matches a database entry of the track at
// `entryIndex`, highest confidence first.
std::vector<AccurateRipOffsetMatch> findAccurateRipOffsets(const AccurateRipOffsetScan& scan,
                                                           const std::vector<AccurateRipPressing>& pressings,
                                                           size_t entryIndex);

}  // namespace cdr
