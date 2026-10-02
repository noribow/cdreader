#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"
#include "cdreader/drive_cache.h"
#include "cdreader/toc.h"

namespace cdr {

struct RipOptions {
    int maxRetries = 5;   // extra attempts per failing read (and re-reads per C2 error sector)
    bool verify = false;  // read every block twice and require identical data

    // Drive read offset correction in samples (one sample = 4 bytes, stereo
    // 16-bit), using the same sign convention as EAC / AccurateRip: with +N
    // the ripper outputs the data the drive reports N samples later.
    int readOffsetSamples = 0;

    // Read with C2 error pointers (#33) and re-read sectors the drive flags.
    // Only for drives that support them: see checkC2(). A drive that rejects
    // such reads anyway makes the ripper fall back to plain reads.
    bool useC2 = false;

    // Drive cache defeat before re-reads (#34): the second read of verify
    // mode, retries, the sector-by-sector fallback and C2 re-reads (not first
    // reads). See checkDriveCache(). With Fua, a drive that rejects the
    // command makes the ripper switch to Flush.
    CacheDefeat cacheDefeat = CacheDefeat::None;
    uint32_t flushSectors = 0;  // sectors per flush (0: flushSectorsForCache(0))
};

struct TrackRipResult {
    int track = 0;
    uint32_t sectors = 0;
    uint32_t unreadableSectors = 0;  // replaced with silence
    uint32_t retries = 0;            // failed or mismatching reads that were retried
    uint32_t paddedSamples = 0;      // samples outside the readable area (offset correction), filled with silence
    uint32_t crc32 = 0;

    // C2 error pointers (#33). Sectors are counted as read from the drive
    // (offset correction may add one sector of the neighbouring track).
    bool c2 = false;              // the track was (at least at first) read with C2 pointers
    uint32_t c2ErrorSectors = 0;  // sectors read with C2 errors; each was re-read on its own
    uint32_t c2Rereads = 0;       // those single-sector re-reads
    uint32_t c2Recovered = 0;     // ... ended by a read without C2 errors
    uint32_t c2Matched = 0;       // ... ended by two identical reads that still had C2 errors
    uint32_t c2Unresolved = 0;    // still C2 errors after the retry budget (the best read was kept)
    // Sectors of the output track (offset corrected, 0 = its first sector)
    // touched by the C2 errors of the unresolved sectors, ascending.
    std::vector<uint32_t> suspiciousSectors;
    std::string c2Fallback;       // C2 reads failed during this track: why (plain reads from then on)

    // Cache defeat (#34): the method at the start of the track, and the FUA
    // commands sent / flushes read before re-reads.
    CacheDefeat cacheDefeat = CacheDefeat::None;
    uint32_t cacheDefeats = 0;
    uint32_t flushSectorsRead = 0;  // sectors read by the flushes
    std::string cacheFallback;      // FUA was rejected during this track: why (flush from then on)

    bool clean() const { return unreadableSectors == 0 && suspiciousSectors.empty(); }
    // "OK", "2 unreadable sector(s)", "3 suspicious sector(s)" or both.
    std::string status() const;
};

// Whether a rip reads with C2 error pointers.
struct C2Availability {
    enum class Mode { Disabled, NotSupported, Supported };
    Mode mode = Mode::Disabled;
    std::string detail;  // NotSupported: why, when the drive did not answer MODE SENSE

    bool usable() const { return mode == Mode::Supported; }
    // "C2 pointers: supported" / "C2 pointers: not supported[ (detail)]" / "C2 pointers: disabled".
    std::string logLine() const;
};

// MODE SENSE page 2Ah when `wanted` (a failure counts as "not supported"),
// otherwise Disabled without sending a command.
C2Availability checkC2(CdDrive& drive, bool wanted);
// The same from capabilities read before (shared with checkDriveCache()).
C2Availability checkC2(const DriveCapabilities& caps, bool wanted);

// Track-relative time of an output sector as EAC prints suspicious
// positions: "h:mm:ss", e.g. "0:01:23".
std::string formatTrackTime(uint32_t sector);

// "Suspicious position 0:01:23 (sector 6225)" or "Suspicious position
// 0:01:23 - 0:01:25 (sectors 6225-6380)" per run of consecutive sectors;
// after `maxLines` runs one line counts the rest.
std::vector<std::string> suspiciousPositionLines(const std::vector<uint32_t>& sectors, size_t maxLines = 50);

// The C2 lines of a track for rip.log, indented by two spaces; empty when
// the track was read without C2 pointers. Shared by the CLI and the app.
std::vector<std::string> c2LogLines(const TrackRipResult& result);

// The cache defeat lines of a track for rip.log, indented by two spaces:
// "  Cache defeat: 12 FUA command(s)" / "  Cache defeat: 3 flush(es),
// 2943 sectors read" and the FUA fallback; empty when nothing was done.
std::vector<std::string> cacheLogLines(const TrackRipResult& result);

class Ripper {
public:
    using SampleSink = std::function<void(const uint8_t* pcm, size_t bytes)>;
    using Progress = std::function<void(uint32_t doneSectors, uint32_t totalSectors)>;

    Ripper(CdDrive& drive, const Toc& toc, RipOptions options)
        : drive_(drive), toc_(toc), options_(options), c2Active_(options.useC2), cacheDefeat_(options.cacheDefeat) {}

    // Reads an audio track and streams its offset-corrected PCM data to `sink`.
    TrackRipResult ripTrack(const Track& track, const SampleSink& sink,
                            const Progress& progress = {});

    // Still reading with C2 pointers: options.useC2 and no fallback since.
    bool c2Active() const { return c2Active_; }
    // Why C2 reads were given up (empty while active or never used).
    const std::string& c2FallbackReason() const { return c2FallbackReason_; }
    // The cache defeat method in use (Flush after the drive rejected FUA).
    CacheDefeat cacheDefeat() const { return cacheDefeat_; }
    // Why FUA was given up (empty while it works or was never used).
    const std::string& cacheFallbackReason() const { return cacheFallbackReason_; }

private:
    struct UnresolvedSector {
        uint32_t lba;
        std::vector<uint8_t> c2;  // C2 bits of the read that was kept
    };

    void readSpan(int64_t lba, uint32_t count, uint8_t* out, TrackRipResult& result);
    bool readBlock(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2, bool reread, TrackRipResult& result);
    void readRange(uint32_t lba, uint32_t count, uint8_t* out, bool reread, TrackRipResult& result);
    bool readOnce(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2, bool reread, TrackRipResult& result);
    void defeatCache(uint32_t lba, uint32_t count, TrackRipResult& result);
    void rereadC2Sector(uint32_t lba, uint8_t* out, const uint8_t* c2, TrackRipResult& result);

    CdDrive& drive_;
    const Toc& toc_;
    RipOptions options_;
    bool c2Active_;
    std::string c2FallbackReason_;
    CacheDefeat cacheDefeat_;
    std::string cacheFallbackReason_;
    Toc::LbaRange readable_;                     // audio run of the current track
    std::vector<uint8_t> flushBuffer_;
    std::vector<uint8_t> raw_;                   // audio + C2 bits of one READ CD
    std::vector<UnresolvedSector> unresolved_;   // of the current span
};

}  // namespace cdr
