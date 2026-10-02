#pragma once

// Drive read cache defeat (#34). Most drives keep the sectors they read in a
// RAM cache (the buffer reported by MODE SENSE page 2Ah). A re-read of the
// same sectors - the second read of --verify, a retry after an error, the
// sector-by-sector fallback or a C2 re-read - is then answered from that
// cache, so it returns the same (possibly wrong) data instead of reading the
// disc again, and a transient error looks "confirmed".
//
// Two ways to make the drive read the disc again:
// - FUA: READ(12) with the Force Unit Access bit and transfer length 0 for
//   the sector about to be re-read (CdDrive::forceUnitAccess()); drives
//   that implement it (Plextor and some others) drop their cached data.
//   Cheap, but many drives accept the command and ignore it, and some reject
//   READ(12) on audio sectors (ILLEGAL REQUEST).
// - Flush: read a region of the disc far away from the target that is larger
//   than the cache, so that the target's sectors are evicted. Works with any
//   drive whose cache is not larger than the size we assume, but costs a few
//   hundred to a few thousand sectors of reading per re-read.
//
// checkDriveCache() decides once per disc which one a rip uses: with the
// "auto" setting by timing re-reads (see the .cpp for the heuristic).

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"
#include "cdreader/clock.h"
#include "cdreader/toc.h"

namespace cdr {

// What the user asks for (CLI --cache, the app's キャッシュ対策 setting).
enum class CacheSetting { Auto, Fua, Flush, None };
// "auto", "fua", "flush", "none".
const char* cacheSettingName(CacheSetting setting);
// Parses one of the names above; false for anything else.
bool parseCacheSetting(const std::string& name, CacheSetting& setting);

// What the ripper does before a re-read (RipOptions::cacheDefeat).
enum class CacheDefeat { None, Fua, Flush };
// "none", "fua", "flush".
const char* cacheDefeatName(CacheDefeat method);

// Assumed cache size when the drive reports none, and the most a flush
// reads (larger reported buffers are capped: CD drives rarely use more than
// a few MB of their buffer for audio, and the flush cost grows with it).
constexpr uint32_t kDefaultCacheKB = 4096;
constexpr uint32_t kMaxCacheKB = 8192;

// Sectors one flush reads for a cache of `cacheKB` KB (0: not reported, the
// default is used; capped at kMaxCacheKB): the cache size plus 10%, at
// least one more read command, so that the whole cache is replaced.
uint32_t flushSectorsForCache(uint32_t cacheKB);

struct FlushRegion {
    uint32_t lba = 0;
    uint32_t count = 0;     // 0: no room for a flush region at all
    bool complete = false;  // count is the full flush size
};

// Where to read `sectors` sectors to evict [target, target + targetCount)
// from the cache, inside `readable` (the audio run of the track: never past
// the lead-out or into a data track). Preferably after the target, ending at
// readable.end (read-ahead past the end of the region cannot bring the
// target back), otherwise from readable.begin (as far before the target as
// possible). When neither side has room for the full size, the larger side
// of the target is used whole (complete false); a target that fills the run
// gets no region (count 0).
FlushRegion selectFlushRegion(const Toc::LbaRange& readable, uint32_t target, uint32_t targetCount, uint32_t sectors);

// Reads the region with plain READ CD commands of up to kMaxSectorsPerRead
// sectors into `scratch` (resized as needed); read errors are ignored.
void flushCache(CdDrive& drive, const FlushRegion& region, std::vector<uint8_t>& scratch);

// Result of checkDriveCache(): what a rip does before re-reads, and why.
struct DriveCacheCheck {
    enum class Result {
        NotRun,       // no detection (setting fua / flush / none)
        NoCache,      // re-reads take as long as reads from the disc
        FuaWorks,     // cached, and a FUA command makes the next read come from the disc
        FuaIgnored,   // cached, FUA accepted but without effect
        FuaRejected,  // the drive rejects READ(12) with FUA (ILLEGAL REQUEST)
        Unknown,      // the test was inconclusive (see detail)
    };

    CacheSetting setting = CacheSetting::None;
    CacheDefeat method = CacheDefeat::None;
    Result result = Result::NotRun;
    bool capabilitiesRead = false;  // MODE SENSE page 2Ah was read (cacheKB is meaningful)
    uint32_t cacheKB = 0;           // reported buffer size (0: not reported)
    uint32_t flushSectors = 0;      // sectors per flush (method flush; also the fallback of fua)
    std::string detail;             // Unknown: why; FuaRejected: the drive's answer

    // Detection timings in microseconds (medians over the test rounds) of a
    // kCacheTestSectors read: right after reading it, after a flush, after FUA.
    uint32_t testSectors = 0;  // 0: no timings
    uint64_t rereadMicros = 0;
    uint64_t flushedMicros = 0;
    uint64_t fuaMicros = 0;
    bool fuaTimed = false;

    // "Drive cache: 2048 KB, FUA works (method: fua)",
    // "Drive cache: 2048 KB, method: none (disabled)", ...
    std::string logLine() const;
    // logLine(), plus the timings of the detection when it ran.
    std::vector<std::string> logLines() const;
};

// Sectors per test read of the detection.
constexpr uint32_t kCacheTestSectors = kMaxSectorsPerRead;

// Decides the cache defeat method of a rip; run once per disc.
// - None: sends no command at all (cacheKB from `caps` when given).
// - Flush: MODE SENSE (unless `caps` is given) for the cache size.
// - Fua: MODE SENSE, then one FUA command: a drive that rejects it gets
//   flush instead (FuaRejected).
// - Auto: MODE SENSE and the timing test on `toc`'s largest audio run
//   (a few seconds: three rounds of reads plus a flush each), using `clock`.
// Never throws; read errors during the test make the result Unknown,
// which uses flush.
DriveCacheCheck checkDriveCache(CdDrive& drive, const Toc& toc, CacheSetting setting, Clock& clock,
                                const DriveCapabilities* caps = nullptr);

}  // namespace cdr
