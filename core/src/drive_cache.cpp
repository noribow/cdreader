#include "cdreader/drive_cache.h"

#include <algorithm>
#include <cstdio>

namespace cdr {

namespace {

// Reading from the disc cannot be faster than this per sector: 200 us is
// 67x, beyond the fastest CD drives (52x - 56x CAV at the outer edge).
constexpr uint64_t kMinDiscMicrosPerSector = 200;
constexpr int kCacheTestRounds = 3;

bool rejected(const ScsiResult& r) { return r.transportOk && r.status == 0x02 && r.sense.key == 0x5; }

uint64_t median(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

std::string millis(uint64_t micros) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f ms", double(micros) / 1000.0);
    return buf;
}

// The largest run of consecutive audio tracks (empty without audio tracks).
Toc::LbaRange largestAudioRun(const Toc& toc) {
    Toc::LbaRange best;
    for (const Track& t : toc.tracks) {
        if (!t.isAudio) continue;
        const Toc::LbaRange r = toc.audioRange(t);
        if (r.end - r.begin > best.end - best.begin) best = r;
    }
    return best;
}

// The timing test of the "auto" setting. Each round, at a position P spread
// over the audio run (1/4, 1/2, 3/4), times a read of n sectors at P
// - right after reading P (cached when the drive caches audio),
// - after FUA for P, the read right after reading P again,
// - after a flush and a read of [P + n, P + 2n) (so that the head is just
//   after P as in the first case and the long seek back from the flush
//   region is not timed): from the disc for any cache not larger than the
//   flush.
// With the medians over the rounds (one disturbed round does not matter):
// - the flushed read faster than the disc can deliver (n * 200 us): the
//   flush did not evict, or the timer is useless: Unknown;
// - the re-read faster than the disc can deliver, or at most a third of
//   the flushed read: cached; at least two thirds: no cache; in between:
//   Unknown;
// - cached: FUA works when the read after it is a disc read, i.e. at least
//   the geometric mean of the cached and the flushed time (and n * 200 us).
void detect(CdDrive& drive, const Toc& toc, Clock& clock, DriveCacheCheck& c) {
    c.method = CacheDefeat::Flush;  // unless proven otherwise
    c.result = DriveCacheCheck::Result::Unknown;
    const uint32_t n = kCacheTestSectors;
    const Toc::LbaRange run = largestAudioRun(toc);
    const uint32_t length = run.end - run.begin;
    // Room for the full flush on one side of every test position.
    if (length < 2 * n + 2 * c.flushSectors) {
        c.detail = "the audio area is too short for the test";
        return;
    }

    std::vector<uint8_t> buffer(size_t(n) * kSectorBytes);
    std::vector<uint8_t> scratch;
    std::vector<uint64_t> reread, flushed, fua;
    bool fuaRejected = false;
    std::string failure;
    auto read = [&](uint32_t lba) {
        const ScsiResult r = drive.readAudio(lba, n, buffer.data());
        if (!r.ok() && failure.empty()) failure = "test read at LBA " + std::to_string(lba) + " failed: " + r.describe();
        return r.ok();
    };
    auto timedRead = [&](uint32_t lba, std::vector<uint64_t>& samples) {
        const uint64_t start = clock.nowMicros();
        if (!read(lba)) return false;
        samples.push_back(clock.nowMicros() - start);
        return true;
    };

    for (int round = 0; round < kCacheTestRounds; ++round) {
        const uint32_t p = run.begin + uint32_t(uint64_t(length - 2 * n) * uint32_t(round + 1) / 4);
        const FlushRegion region = selectFlushRegion(run, p, 2 * n, c.flushSectors);
        if (!read(p) || !timedRead(p, reread)) continue;
        if (!fuaRejected) {
            const ScsiResult r = drive.forceUnitAccess(p);
            if (rejected(r)) {
                fuaRejected = true;
                c.detail = r.describe();
            } else if (r.ok()) {
                if (!timedRead(p, fua)) continue;
            } else if (c.detail.empty()) {
                c.detail = "FUA failed: " + r.describe();
            }
        }
        flushCache(drive, region, scratch);
        if (!read(p + n)) continue;
        timedRead(p, flushed);
    }

    if (reread.size() < 2 || flushed.size() < 2) {
        c.detail = failure.empty() ? "too few test reads" : failure;
        return;
    }
    c.testSectors = n;
    c.rereadMicros = median(reread);
    c.flushedMicros = median(flushed);
    c.fuaTimed = fua.size() >= 2 && !fuaRejected;
    if (c.fuaTimed) c.fuaMicros = median(fua);

    const uint64_t bound = uint64_t(n) * kMinDiscMicrosPerSector;
    const uint64_t hit = c.rereadMicros;
    const uint64_t miss = c.flushedMicros;
    if (miss < bound) {
        c.detail = "reads after a flush of " + std::to_string(c.flushSectors) +
                   " sectors were too fast for the disc (larger cache?)";
        return;
    }
    const bool cached = hit < bound || miss >= 3 * hit;
    const bool noCache = !cached && 2 * miss <= 3 * hit;
    if (!cached && !noCache) {
        c.detail = "ambiguous timings";
        return;
    }
    if (noCache) {
        c.detail.clear();
        c.result = DriveCacheCheck::Result::NoCache;
        c.method = CacheDefeat::None;
        return;
    }
    if (fuaRejected) {
        c.result = DriveCacheCheck::Result::FuaRejected;
        return;
    }
    const bool fuaWorks = c.fuaTimed && c.fuaMicros >= bound && double(c.fuaMicros) * double(c.fuaMicros) >= double(hit) * double(miss);
    c.result = fuaWorks ? DriveCacheCheck::Result::FuaWorks : DriveCacheCheck::Result::FuaIgnored;
    c.method = fuaWorks ? CacheDefeat::Fua : CacheDefeat::Flush;
    if (fuaWorks) c.detail.clear();
}

}  // namespace

const char* cacheSettingName(CacheSetting setting) {
    switch (setting) {
        case CacheSetting::Auto: return "auto";
        case CacheSetting::Fua: return "fua";
        case CacheSetting::Flush: return "flush";
        case CacheSetting::None: break;
    }
    return "none";
}

bool parseCacheSetting(const std::string& name, CacheSetting& setting) {
    for (CacheSetting s : {CacheSetting::Auto, CacheSetting::Fua, CacheSetting::Flush, CacheSetting::None}) {
        if (name == cacheSettingName(s)) {
            setting = s;
            return true;
        }
    }
    return false;
}

const char* cacheDefeatName(CacheDefeat method) {
    switch (method) {
        case CacheDefeat::Fua: return "fua";
        case CacheDefeat::Flush: return "flush";
        case CacheDefeat::None: break;
    }
    return "none";
}

uint32_t flushSectorsForCache(uint32_t cacheKB) {
    const uint32_t kb = cacheKB == 0 ? kDefaultCacheKB : std::min(cacheKB, kMaxCacheKB);
    const uint32_t sectors = uint32_t((uint64_t(kb) * 1024 + kSectorBytes - 1) / kSectorBytes);
    return sectors + std::max(sectors / 10, kMaxSectorsPerRead);
}

FlushRegion selectFlushRegion(const Toc::LbaRange& readable, uint32_t target, uint32_t targetCount, uint32_t sectors) {
    FlushRegion region;
    if (readable.end <= readable.begin || sectors == 0) return region;
    const uint32_t first = std::clamp(target, readable.begin, readable.end);
    const uint32_t last = uint32_t(std::clamp<uint64_t>(uint64_t(target) + targetCount, first, readable.end));
    const uint32_t after = readable.end - last;
    const uint32_t before = first - readable.begin;
    if (after >= sectors) {
        region = {readable.end - sectors, sectors, true};
    } else if (before >= sectors) {
        region = {readable.begin, sectors, true};
    } else if (after >= before) {
        region = {last, after, false};
    } else {
        region = {readable.begin, before, false};
    }
    return region;
}

void flushCache(CdDrive& drive, const FlushRegion& region, std::vector<uint8_t>& scratch) {
    scratch.resize(size_t(kMaxSectorsPerRead) * kSectorBytes);
    for (uint32_t done = 0; done < region.count;) {
        const uint32_t count = std::min(kMaxSectorsPerRead, region.count - done);
        drive.readAudio(region.lba + done, count, scratch.data());  // errors do not matter here
        done += count;
    }
}

std::string DriveCacheCheck::logLine() const {
    std::string size;
    if (capabilitiesRead && cacheKB > 0) {
        size = std::to_string(cacheKB) + " KB";
        if (cacheKB > kMaxCacheKB && method == CacheDefeat::Flush)
            size += " (flush capped at " + std::to_string(kMaxCacheKB) + " KB)";
    } else if (capabilitiesRead) {
        size = "size not reported";
    } else if (setting != CacheSetting::None) {
        size = "size unknown";
    }
    if (method == CacheDefeat::Flush && (!capabilitiesRead || cacheKB == 0))
        size += " (assuming " + std::to_string(kDefaultCacheKB) + " KB)";

    std::string what;
    switch (result) {
        case Result::NotRun: break;
        case Result::NoCache: what = "no audio caching detected"; break;
        case Result::FuaWorks: what = "caches audio, FUA works"; break;
        case Result::FuaIgnored:
            what = "caches audio, FUA ignored" + (detail.empty() ? "" : " (" + detail + ")");
            break;
        case Result::FuaRejected:
            what = std::string(setting == CacheSetting::Auto ? "caches audio, " : "") + "FUA rejected by the drive" +
                   (detail.empty() ? "" : " (" + detail + ")");
            break;
        case Result::Unknown: what = "detection inconclusive: " + detail; break;
    }

    std::string m = std::string("method: ") + cacheDefeatName(method);
    if (setting == CacheSetting::None) m += " (disabled)";
    else if (setting != CacheSetting::Auto && result == Result::NotRun) m += " (forced)";
    if (method == CacheDefeat::Flush) m += ", " + std::to_string(flushSectors) + " sectors per flush";

    std::string line = "Drive cache: ";
    if (!size.empty()) line += size + ", ";
    if (what.empty()) return line + m;
    return line + what + " (" + m + ")";
}

std::vector<std::string> DriveCacheCheck::logLines() const {
    std::vector<std::string> lines{logLine()};
    if (testSectors > 0) {
        std::string t = "Cache test (" + std::to_string(testSectors) + " sectors): re-read " + millis(rereadMicros) +
                        ", after flush " + millis(flushedMicros);
        if (fuaTimed) t += ", after FUA " + millis(fuaMicros);
        lines.push_back(t);
    }
    return lines;
}

DriveCacheCheck checkDriveCache(CdDrive& drive, const Toc& toc, CacheSetting setting, Clock& clock,
                                const DriveCapabilities* caps) {
    DriveCacheCheck c;
    c.setting = setting;
    DriveCapabilities read;
    if (caps == nullptr && setting != CacheSetting::None) {
        read = drive.readCapabilities();
        caps = &read;
    }
    if (caps != nullptr && caps->valid) {
        c.capabilitiesRead = true;
        c.cacheKB = caps->bufferKB;
    }
    c.flushSectors = flushSectorsForCache(c.cacheKB);

    switch (setting) {
        case CacheSetting::None:
            c.method = CacheDefeat::None;
            break;
        case CacheSetting::Flush:
            c.method = CacheDefeat::Flush;
            break;
        case CacheSetting::Fua: {
            c.method = CacheDefeat::Fua;
            const ScsiResult r = drive.forceUnitAccess(largestAudioRun(toc).begin);
            if (rejected(r)) {
                c.result = DriveCacheCheck::Result::FuaRejected;
                c.detail = r.describe();
                c.method = CacheDefeat::Flush;
            }
            break;
        }
        case CacheSetting::Auto:
            detect(drive, toc, clock, c);
            break;
    }
    return c;
}

}  // namespace cdr
