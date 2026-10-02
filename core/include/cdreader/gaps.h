#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"
#include "cdreader/toc.h"

// Pregap (INDEX 00), index point (INDEX 02+) and HTOA detection from the Q
// sub-channel (#25).
//
// The TOC only lists where each track's INDEX 01 is. Between two tracks the
// disc usually has a pregap (INDEX 00 of the next track, often 2 seconds of
// silence, sometimes audio such as applause or a hidden count-in), which the
// TOC counts as the end of the previous track. Every sector's Q frame says
// which track and index it belongs to; as track * 100 + index never decreases
// along the disc, the first sector of INDEX 00 is found by a search that only
// reads a few dozen sectors per track (galloping back from INDEX 01, then
// bisecting). Each boundary is confirmed by reading its neighbours again; when
// that disagrees (a drive whose Q frames carry no CRC returned a bad one), the
// search is repeated with two agreeing reads per position.
//
// Each position is "probed": a READ CD of a few sectors around it, keeping
// the Q frames whose own absolute time lies inside the interval still being
// searched. When that finds nothing (mode 2 / 3 frames, CRC errors, or a drive
// returning the Q frame of a neighbouring sector, #41), the probe reads a
// wider window, then windows further away, then asks the other Q sources
// (raw P-W, READ SUB-CHANNEL) before giving up. A constant offset between the
// sector read and the position in its Q frame ("Q delay", seen on many
// drives) is measured on the position frames read with READ CD (a frame
// whose CRC is wrong is never counted; 90 % must agree) and the reads are
// placed to make up for it. The positions found are always those of the Q frames (the time
// written in them), so the delay does not shift the result. Up to 3 sectors
// whose Q frames never carry a position (a mode 2 / 3 frame right at the
// boundary) leave the boundary known only to within them: the first sector
// seen in the new index is taken and the track's detail says so.
//
// HTOA ("hidden track one audio"): on a normal disc track 1 starts at LBA 0
// (its 2-second pregap lies before LBA 0 and cannot be read as audio). When
// the TOC puts track 1 later, the sectors from LBA 0 up to it are the pregap
// of track 1 (INDEX 00) and hold audio that players only reach by rewinding:
// the hidden track. Its length is known from the TOC alone; the Q frames at
// LBA 0 and just before track 1 confirm it.
namespace cdr {

// How Q frames are read.
enum class QSource {
    Auto,             // the first of the following that works on the drive
    FormattedQ,       // READ CD, sub-channel selection 010b (16 bytes per sector)
    RawPW,            // READ CD, sub-channel selection 001b (96 bytes, Q de-interleaved here, CRC checked)
    CurrentPosition,  // READ CD of the sector, then READ SUB-CHANNEL format 01h (slow; last resort)
};
std::string describeQSource(QSource source);

struct TrackIndexes {
    enum class Status {
        Detected,  // pregapSectors (and laterIndexes) are known
        Unknown,   // could not be detected (`detail`): treated as "no pregap"
        Skipped,   // not searched (`detail`, e.g. after a data track)
    };
    int track = 0;
    uint32_t index01Lba = 0;           // from the TOC
    Status status = Status::Unknown;
    uint32_t pregapSectors = 0;        // INDEX 00 .. INDEX 01 (track 1: the HTOA)
    std::vector<uint32_t> laterIndexes;  // LBAs of INDEX 02, 03, ...
    std::string detail;

    bool detected() const { return status == Status::Detected; }
    uint32_t index00Lba() const { return index01Lba - pregapSectors; }
};

// What the Q frames read during gap detection looked like (#41): every frame
// received is counted once, as used or under the reason it was not.
struct QFrameStats {
    unsigned frames = 0;         // Q frames received (one per sector read / READ SUB-CHANNEL)
    unsigned used = 0;           // usable position frames inside the interval searched
    unsigned otherAdr = 0;       // mode 2 / 3 (MCN / ISRC): no position
    unsigned crcErrors = 0;      // CRC mismatch
    unsigned badBcd = 0;         // mode 1 frame with malformed BCD / out of range numbers
    unsigned farOff = 0;         // position more than 10 sectors from the sector read
    unsigned outsideWindow = 0;  // usable, but outside the interval searched
    unsigned other = 0;          // no mode (e.g. all zero), or not confirmed by a second read
    unsigned failedCommands = 0; // READ CD / READ SUB-CHANNEL that failed
    // Position in the frame - sector read, of the mode 1 frames with a
    // position (also those too far off), by delta.
    std::map<int32_t, unsigned> deltas;

    unsigned rejected() const { return otherAdr + crcErrors + badBcd + farOff + outsideWindow + other; }
    void add(const QFrameStats& o);
    // "12 Q frames, 6 ADR 2/3, 6 out of window, position delta +1..+2" (for a failed probe).
    std::string summary() const;
    // "Q frames: N read, M used; rejected: ...; position delta min / max / median" (rip.log).
    std::string logLine() const;
};

struct GapDetectionOptions {
    QSource source = QSource::Auto;
    // READ CD / READ SUB-CHANNEL commands for the whole disc. A typical disc
    // needs 15-30 per track; tracks left when the budget runs out stay Unknown.
    unsigned maxReads = 4000;
    bool laterIndexes = true;  // also look for INDEX 02+ (one read per track when there are none)
    // Called before each track is searched (track number, last track number).
    std::function<void(int, int)> progress;
};

struct DiscGaps {
    enum class Status {
        NotRun,       // detection disabled: only the HTOA from the TOC is known
        Detected,     // every audio track was searched
        Partial,      // some tracks could not be searched (see their detail)
        Unsupported,  // the drive returns no Q frames: no gaps known
    };
    Status status = Status::NotRun;
    QSource source = QSource::Auto;      // the method used
    std::vector<TrackIndexes> tracks;    // audio tracks, TOC order (empty when NotRun / Unsupported)
    uint32_t htoaSectors = 0;            // from the TOC: track 1 starts at this LBA (> 0: HTOA)
    bool htoaConfirmed = false;          // the Q frames show track 1 INDEX 00 at LBA 0
    unsigned reads = 0;                  // commands sent
    double seconds = 0;
    QFrameStats qFrames;                 // what the Q frames looked like (diagnostics, #41)
    int32_t qDelay = 0;                  // constant Q delay found and made up for (sectors; 0: none)
    unsigned widerProbes = 0;            // probes answered only by a wider / shifted read
    std::map<QSource, unsigned> fallbackProbes;  // probes answered only by another Q source
    unsigned unresolvedSectors = 0;      // boundaries known to +-1 sector only (no position frame there)
    std::string detail;

    bool hasHtoa() const { return htoaSectors > 0; }
    const TrackIndexes* find(int track) const;
    // The detected pregap of `track` (track 1: the HTOA, from the TOC), 0 when unknown.
    uint32_t pregap(int track) const;
    // LBAs of INDEX 02, 03, ... of `track` (empty when unknown).
    std::vector<uint32_t> laterIndexes(int track) const;
    // rip.log / `cdreader toc` lines (the CLI and Android share them).
    std::vector<std::string> logLines() const;
};

// The HTOA of a disc from its TOC: sectors before track 1 when track 1 is an
// audio track starting after LBA 0, else 0.
uint32_t htoaSectors(const Toc& toc);
// The HTOA as a track to rip: number 0, LBA 0, htoaSectors(toc) long.
Track htoaTrack(const Toc& toc);
// Without detection: status NotRun and the HTOA from the TOC.
DiscGaps gapsFromToc(const Toc& toc);

// Searches the pregap and index points of every audio track. Never throws for
// drive errors: what could not be found is reported in the statuses.
DiscGaps detectGaps(CdDrive& drive, const Toc& toc, const GapDetectionOptions& options = {});

}  // namespace cdr
