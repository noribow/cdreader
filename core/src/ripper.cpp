#include "cdreader/ripper.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "cdreader/crc32.h"

namespace cdr {

namespace {

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

size_t countBits(const uint8_t* c2) {
    size_t n = 0;
    for (size_t i = 0; i < kC2BytesPerSector; ++i)
        for (uint8_t b = c2[i]; b; b &= uint8_t(b - 1)) ++n;
    return n;
}

bool anyBit(const uint8_t* c2) {
    for (size_t i = 0; i < kC2BytesPerSector; ++i)
        if (c2[i]) return true;
    return false;
}

}  // namespace

// --- Reporting ---------------------------------------------------------------

std::string TrackRipResult::status() const {
    std::string s;
    if (unreadableSectors) s = std::to_string(unreadableSectors) + " unreadable sector(s)";
    if (!suspiciousSectors.empty())
        s += (s.empty() ? "" : ", ") + std::to_string(suspiciousSectors.size()) + " suspicious sector(s)";
    return s.empty() ? "OK" : s;
}

std::string C2Availability::logLine() const {
    switch (mode) {
        case Mode::Supported: return "C2 pointers: supported";
        case Mode::NotSupported: return "C2 pointers: not supported" + (detail.empty() ? "" : " (" + detail + ")");
        case Mode::Disabled: break;
    }
    return "C2 pointers: disabled";
}

C2Availability checkC2(CdDrive& drive, bool wanted) {
    if (!wanted) return {};
    return checkC2(drive.readCapabilities(), true);
}

C2Availability checkC2(const DriveCapabilities& caps, bool wanted) {
    C2Availability a;
    if (!wanted) return a;
    a.mode = caps.c2Pointers ? C2Availability::Mode::Supported : C2Availability::Mode::NotSupported;
    if (!caps.valid) a.detail = caps.error;
    return a;
}

std::string formatTrackTime(uint32_t sector) {
    const uint32_t seconds = sector / kSectorsPerSecond;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u:%02u:%02u", unsigned(seconds / 3600), unsigned(seconds / 60 % 60),
                  unsigned(seconds % 60));
    return buf;
}

std::vector<std::string> suspiciousPositionLines(const std::vector<uint32_t>& sectors, size_t maxLines) {
    std::vector<std::string> lines;
    size_t runs = 0;
    for (size_t i = 0; i < sectors.size();) {
        size_t j = i;
        while (j + 1 < sectors.size() && sectors[j + 1] == sectors[j] + 1) ++j;
        if (runs++ < maxLines) {
            const uint32_t first = sectors[i];
            const uint32_t last = sectors[j];
            if (first == last)
                lines.push_back("Suspicious position " + formatTrackTime(first) + " (sector " + std::to_string(first) +
                                ")");
            else
                lines.push_back("Suspicious position " + formatTrackTime(first) + " - " + formatTrackTime(last) +
                                " (sectors " + std::to_string(first) + "-" + std::to_string(last) + ")");
        }
        i = j + 1;
    }
    if (runs > maxLines) lines.push_back("... and " + std::to_string(runs - maxLines) + " more suspicious position(s)");
    return lines;
}

std::vector<std::string> c2LogLines(const TrackRipResult& r) {
    std::vector<std::string> lines;
    if (!r.c2) return lines;
    if (r.c2ErrorSectors == 0) {
        lines.push_back("  C2 errors: none");
    } else {
        lines.push_back("  C2 errors: " + std::to_string(r.c2ErrorSectors) + " sector(s), " +
                        std::to_string(r.c2Rereads) + " re-read(s) (" + std::to_string(r.c2Recovered) +
                        " recovered, " + std::to_string(r.c2Matched) + " identical re-reads with C2, " +
                        std::to_string(r.c2Unresolved) + " unresolved)");
    }
    if (!r.c2Fallback.empty()) lines.push_back("  C2 reads given up: " + r.c2Fallback);
    for (const std::string& l : suspiciousPositionLines(r.suspiciousSectors)) lines.push_back("  " + l);
    return lines;
}

std::vector<std::string> cacheLogLines(const TrackRipResult& r) {
    std::vector<std::string> lines;
    if (r.cacheDefeats > 0) {
        if (r.flushSectorsRead > 0)
            lines.push_back("  Cache defeat: " + std::to_string(r.cacheDefeats) + " flush(es), " +
                            std::to_string(r.flushSectorsRead) + " sectors read");
        else
            lines.push_back("  Cache defeat: " + std::to_string(r.cacheDefeats) + " FUA command(s)");
    }
    if (!r.cacheFallback.empty()) lines.push_back("  FUA given up: " + r.cacheFallback);
    return lines;
}

std::vector<std::string> slowdownLogLines(const TrackRipResult& r) {
    std::vector<std::string> lines;
    if (!r.slowdownRecovered.empty() || r.slowdownFailed) {
        std::string line;
        // Fastest speed first: "2 sector(s) re-read at 8x, 1 at 4x".
        for (auto it = r.slowdownRecovered.rbegin(); it != r.slowdownRecovered.rend(); ++it)
            line += line.empty() ? std::to_string(it->second) + " sector(s) re-read at " + std::to_string(it->first) + "x"
                                 : ", " + std::to_string(it->second) + " at " + std::to_string(it->first) + "x";
        if (r.slowdownFailed)
            line += (line.empty() ? "" : ", ") + std::to_string(r.slowdownFailed) +
                    (line.empty() ? " sector(s) not recovered at lower speeds" : " not recovered");
        lines.push_back("  Slowdown: " + line);
    }
    if (!r.slowdownFallback.empty()) lines.push_back("  Slowdown given up: " + r.slowdownFallback);
    return lines;
}

std::vector<int> slowdownSteps(const RipOptions& options) {
    // The speed now: the lower of the one set and the one reported.
    uint32_t now = options.speedKBps == kMaxSpeedKBps ? 0 : options.speedKBps;
    if (options.currentSpeedKBps && (now == 0 || options.currentSpeedKBps < now)) now = options.currentSpeedKBps;
    std::vector<int> steps;
    for (int factor : options.slowdownSpeeds) {
        if (factor <= 0 || factor > kMaxSpeedFactor) continue;
        if (now && speedFactor(now) <= factor) continue;  // not slower than now
        if (!steps.empty() && steps.back() <= factor) continue;  // descending only
        steps.push_back(factor);
    }
    return steps;
}

std::vector<std::string> ReadSpeedSetup::logLines() const {
    std::vector<std::string> lines = {info.logLine()};
    if (change.result != SpeedChange::Result::NotRequested) lines.push_back(change.logLine());
    lines.push_back(slowdownLine);
    return lines;
}

ReadSpeedSetup setUpReadSpeed(CdDrive& drive, const DriveCapabilities& caps, const SpeedSetting& speed,
                              RipOptions& options) {
    ReadSpeedSetup setup;
    setup.info = driveSpeedInfo(caps, readDiscPerformance(drive));
    DriveCapabilities after;
    setup.change = setReadSpeed(drive, speed, caps, &after);
    options.speedKBps = setup.change.effectiveKBps();
    options.currentSpeedKBps = driveSpeedInfo(after, {}).currentReadKBps;
    std::string unavailable;
    if (setup.change.result == SpeedChange::Result::Rejected) unavailable = "the drive rejects SET CD SPEED";
    setup.slowdownLine = slowdownLogLine(options, unavailable);
    if (!unavailable.empty()) options.slowdown = false;
    return setup;
}

std::string slowdownLogLine(const RipOptions& options, const std::string& unavailable) {
    const std::string head = "Slowdown on read errors: ";
    if (!options.slowdown) return head + "disabled";
    if (!unavailable.empty()) return head + "not possible (" + unavailable + ")";
    const std::vector<int> steps = slowdownSteps(options);
    if (steps.empty()) return head + "none below the current speed";
    std::string list;
    for (int f : steps) list += (list.empty() ? "" : ", ") + std::to_string(f) + "x";
    return head + list;
}

std::vector<std::string> trackRipLogLines(const std::string& name, const TrackRipResult& r) {
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", r.crc32);
    std::string first = name + "  CRC32 " + crc + "  retries " + std::to_string(r.retries) + "  " + r.status();
    if (r.paddedSamples)
        first += "  (" + std::to_string(r.paddedSamples) + " samples outside the disc padded with silence)";
    std::vector<std::string> lines = {first};
    if (r.readMicros) lines.push_back("  Read speed: " + formatMeasuredSpeed(r.sectors, r.readMicros));
    for (std::string& l : c2LogLines(r)) lines.push_back(std::move(l));
    for (std::string& l : cacheLogLines(r)) lines.push_back(std::move(l));
    for (std::string& l : slowdownLogLines(r)) lines.push_back(std::move(l));
    return lines;
}

// --- Ripper ------------------------------------------------------------------

// With a read offset the track's samples no longer start on a sector
// boundary: read the sectors covering [start + offset, end + offset) and
// emit exactly the track's length from the right byte position. Parts that
// fall outside the readable audio area (before the first audio track or past
// the lead-out) cannot be read by most drives and are filled with silence,
// as other rippers do when overreading is not available.
TrackRipResult Ripper::ripTrack(const Track& track, const SampleSink& sink, const Progress& progress) {
    TrackRipResult result;
    result.track = track.number;
    result.sectors = track.lengthSectors;
    result.c2 = c2Active_;
    result.cacheDefeat = cacheDefeat_;
    result.slowdown = slowdownActive_;
    const std::string cacheFallbackBefore = cacheFallbackReason_;
    trackReadMicros_ = 0;

    const Toc::LbaRange readable = toc_.audioRange(track);
    readable_ = readable;
    const int64_t firstSample = int64_t(track.startLba) * kSamplesPerSector + options_.readOffsetSamples;
    const int64_t firstSector = floorDiv(firstSample, kSamplesPerSector);
    size_t skipBytes = size_t(firstSample - firstSector * kSamplesPerSector) * kBytesPerSample;
    const uint64_t totalBytes = uint64_t(track.lengthSectors) * kSectorBytes;
    const uint32_t sectorsToRead = track.lengthSectors + (skipBytes ? 1 : 0);

    const int64_t trackSamples = int64_t(track.lengthSectors) * kSamplesPerSector;
    const int64_t readableFirst = int64_t(readable.begin) * kSamplesPerSector;
    const int64_t readableEnd = int64_t(readable.end) * kSamplesPerSector;
    const int64_t overlap = std::min(firstSample + trackSamples, readableEnd) - std::max(firstSample, readableFirst);
    result.paddedSamples = uint32_t(trackSamples - std::max<int64_t>(0, overlap));

    // C2 bits of sectors that stayed bad, mapped to the bytes of the output
    // track: the read offset shifts them, possibly into the next sector.
    const int64_t trackFirstByte = firstSample * kBytesPerSample;
    std::set<uint32_t> suspicious;
    auto mapUnresolved = [&]() {
        for (const UnresolvedSector& u : unresolved_) {
            const bool whole = !anyBit(u.c2.data());  // verify mode: an unconfirmed C2-free read
            for (size_t byte = 0; byte < kSectorBytes; ++byte) {
                if (!whole && !(u.c2[byte >> 3] & (0x80 >> (byte & 7)))) continue;
                const int64_t pos = int64_t(u.lba) * kSectorBytes + int64_t(byte) - trackFirstByte;
                if (pos >= 0 && uint64_t(pos) < totalBytes) suspicious.insert(uint32_t(pos / kSectorBytes));
            }
        }
        unresolved_.clear();
    };

    Crc32 crc;
    std::vector<uint8_t> buffer(size_t(kMaxSectorsPerRead) * kSectorBytes);
    uint64_t emitted = 0;
    uint32_t sectorsDone = 0;
    if (progress) progress(0, track.lengthSectors);
    while (sectorsDone < sectorsToRead) {
        // READ CD with C2 bits moves 2646 bytes per sector: fewer per command.
        const uint32_t blockSectors = c2Active_ ? kMaxSectorsPerC2Read : kMaxSectorsPerRead;
        const uint32_t count = std::min(blockSectors, sectorsToRead - sectorsDone);
        const uint64_t started = clock_ ? clock_->nowMicros() : 0;
        readSpan(firstSector + sectorsDone, count, buffer.data(), result);
        if (clock_) trackReadMicros_ += clock_->nowMicros() - started;
        mapUnresolved();
        sectorsDone += count;

        const size_t available = size_t(count) * kSectorBytes - skipBytes;
        const size_t bytes = size_t(std::min<uint64_t>(available, totalBytes - emitted));
        crc.update(buffer.data() + skipBytes, bytes);
        sink(buffer.data() + skipBytes, bytes);
        emitted += bytes;
        skipBytes = 0;
        if (progress) progress(std::min(sectorsDone, track.lengthSectors), track.lengthSectors);
    }
    result.crc32 = crc.value();
    result.suspiciousSectors.assign(suspicious.begin(), suspicious.end());
    if (result.c2 && !c2Active_) result.c2Fallback = c2FallbackReason_;
    if (cacheFallbackReason_ != cacheFallbackBefore) result.cacheFallback = cacheFallbackReason_;
    if (result.slowdown && !slowdownActive_) result.slowdownFallback = slowdownFallbackReason_;
    result.readMicros = trackReadMicros_;
    return result;
}

// Fills `count` sectors starting at `lba` (which may be negative): sectors
// inside the track's audio run come from the drive, the rest are silence.
void Ripper::readSpan(int64_t lba, uint32_t count, uint8_t* out, TrackRipResult& result) {
    const int64_t begin = std::max<int64_t>(lba, readable_.begin);
    const int64_t end = std::min<int64_t>(lba + count, readable_.end);
    std::memset(out, 0, size_t(count) * kSectorBytes);
    if (begin < end)
        readRange(uint32_t(begin), uint32_t(end - begin), out + size_t(begin - lba) * kSectorBytes, false, result);
    // Slowed down for a problem in this block: back to the speed of the rip.
    if (slowedKBps_) restoreSpeed(uint32_t(std::max<int64_t>(begin, 0)));
}

// --- Slowdown on read errors (#49) --------------------------------------------

std::vector<int> Ripper::currentSlowdownSteps() const {
    RipOptions now = options_;
    if (slowedKBps_) now.speedKBps = slowedKBps_;
    return slowdownSteps(now);
}

// SET CD SPEED; a drive that fails it cannot slow down: given up for the
// rest of the disc (the speed it reads at stays unknown, so no restore).
bool Ripper::setSpeed(uint16_t kBps, uint32_t lba) {
    const ScsiResult r = drive_.setCdSpeed(kBps);
    if (r.ok()) return true;
    slowdownActive_ = false;
    slowedKBps_ = 0;
    slowdownFallbackReason_ = "SET CD SPEED to " + (kBps == kMaxSpeedKBps ? std::string("max") : formatSpeed(kBps)) +
                              " failed at LBA " + std::to_string(lba) + " (" + r.describe() +
                              "), no slowdown from there on";
    return false;
}

void Ripper::restoreSpeed(uint32_t lba) {
    slowedKBps_ = 0;
    setSpeed(options_.speedKBps, lba);
}

// A sector that stayed unreadable at the rip's speed: reads it at each
// lower speed (two attempts each) until one works. True when it did; the
// data and C2 bits are in `out` / `c2`. The speed stays lowered until the
// end of the block (readSpan restores it).
bool Ripper::slowDownForSector(uint32_t lba, uint8_t* out, uint8_t* c2, TrackRipResult& result) {
    if (!slowdownActive_) return false;
    bool tried = false;
    for (int factor : currentSlowdownSteps()) {
        if (!setSpeed(speedKBps(factor), lba)) break;
        slowedKBps_ = speedKBps(factor);
        tried = true;
        if (readBlock(lba, 1, out, c2, true, result, std::min(options_.maxRetries, 1))) {
            ++result.slowdownRecovered[factor];
            return true;
        }
    }
    if (tried) ++result.slowdownFailed;
    return false;
}

// Makes the drive read [lba, lba + count) from the disc on the next read
// instead of answering from its cache (#34).
void Ripper::defeatCache(uint32_t lba, uint32_t count, TrackRipResult& result) {
    if (cacheDefeat_ == CacheDefeat::Fua) {
        const ScsiResult r = drive_.forceUnitAccess(lba);
        if (!(r.transportOk && r.status == 0x02 && r.sense.key == 0x5)) {
            // Other failures (a medium error, ...) do not mean that the
            // drive cannot do it: keep FUA.
            ++result.cacheDefeats;
            return;
        }
        cacheDefeat_ = CacheDefeat::Flush;
        cacheFallbackReason_ = "READ(12) with FUA rejected at LBA " + std::to_string(lba) + " (" + r.describe() +
                               "), flush from there on";
    }
    if (cacheDefeat_ != CacheDefeat::Flush) return;
    const uint32_t sectors = options_.flushSectors ? options_.flushSectors : flushSectorsForCache(0);
    const FlushRegion region = selectFlushRegion(readable_, lba, count, sectors);
    if (region.count == 0) return;
    flushCache(drive_, region, flushBuffer_);
    ++result.cacheDefeats;
    result.flushSectorsRead += region.count;
}

// One READ CD of `count` sectors into `out`, with the C2 bits into `c2`
// (count * kC2BytesPerSector bytes, all zero when C2 pointers are not used).
// `reread`: these sectors were read before, defeat the drive cache first.
// A drive that rejects the C2 read (ILLEGAL REQUEST, or less data) while a
// plain read of the same sectors works cannot deliver C2 bits: plain reads
// from then on, for the rest of the disc.
bool Ripper::readOnce(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2, bool reread,
                      TrackRipResult& result) {
    if (reread && cacheDefeat_ != CacheDefeat::None) defeatCache(lba, count, result);
    std::memset(c2, 0, size_t(count) * kC2BytesPerSector);
    if (!c2Active_) return drive_.readAudio(lba, count, out).ok();

    raw_.resize(size_t(count) * kSectorWithC2Bytes);
    const ScsiResult r = drive_.readAudioWithC2(lba, count, raw_.data());
    if (r.ok()) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* sector = raw_.data() + size_t(i) * kSectorWithC2Bytes;
            std::memcpy(out + size_t(i) * kSectorBytes, sector, kSectorBytes);
            std::memcpy(c2 + size_t(i) * kC2BytesPerSector, sector + kSectorBytes, kC2BytesPerSector);
        }
        return true;
    }
    const bool rejected = r.shortRead || (r.transportOk && r.status == 0x02 && r.sense.key == 0x5);
    if (!rejected || !drive_.readAudio(lba, count, out).ok()) return false;
    c2Active_ = false;
    c2FallbackReason_ = "READ CD with C2 error pointers failed at LBA " + std::to_string(lba) + " (" + r.describe() +
                        "), plain reads from there on";
    return true;
}

// Reads a block with retries; in verify mode a block only counts once two
// consecutive reads return identical data (their C2 bits are combined).
// Every read but the first (unless `reread`) defeats the drive cache.
bool Ripper::readBlock(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2, bool reread,
                       TrackRipResult& result, int retries) {
    const size_t bytes = size_t(count) * kSectorBytes;
    std::vector<uint8_t> check, checkC2;
    if (options_.verify) {
        check.resize(bytes);
        checkC2.resize(size_t(count) * kC2BytesPerSector);
    }

    for (int attempt = 0; attempt <= retries; ++attempt) {
        if (attempt > 0) ++result.retries;
        if (!readOnce(lba, count, out, c2, reread || attempt > 0, result)) continue;
        if (!options_.verify) return true;
        if (!readOnce(lba, count, check.data(), checkC2.data(), true, result)) continue;
        if (std::memcmp(out, check.data(), bytes) == 0) {
            for (size_t i = 0; i < checkC2.size(); ++i) c2[i] |= checkC2[i];
            return true;
        }
    }
    return false;
}

// Falls back to sector-by-sector reads when a multi-sector block keeps
// failing, so that a single bad sector does not silence its neighbours.
// Sectors of a successful read that carry C2 errors are re-read one by one.
// `reread`: the sectors were read before (the fallback), so even the first
// read defeats the drive cache.
void Ripper::readRange(uint32_t lba, uint32_t count, uint8_t* out, bool reread, TrackRipResult& result) {
    std::vector<uint8_t> c2(size_t(count) * kC2BytesPerSector);
    const bool ok = readBlock(lba, count, out, c2.data(), reread, result, options_.maxRetries) ||
                    (count == 1 && slowDownForSector(lba, out, c2.data(), result));
    if (ok) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* bits = c2.data() + size_t(i) * kC2BytesPerSector;
            if (!anyBit(bits)) continue;
            ++result.c2ErrorSectors;
            rereadC2Sector(lba + i, out + size_t(i) * kSectorBytes, bits, result);
        }
        return;
    }
    if (count == 1) {
        std::memset(out, 0, kSectorBytes);
        ++result.unreadableSectors;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) readRange(lba + i, 1, out + size_t(i) * kSectorBytes, true, result);
}

// Re-reads a sector the drive flagged with C2 errors, up to maxRetries
// times. It ends with
// - a read without C2 errors (verify mode: confirmed by an identical read), or
// - two consecutive identical reads that still have C2 errors: the drive
//   returns the same data every time (or reports false C2 errors), so more
//   reads are unlikely to change anything. In verify mode both must be
//   re-reads; otherwise the read that flagged the sector counts as well.
// Without such an end, with slowdown (#49) the same is tried at each lower
// speed (budget 2 re-reads; a speed change needs two identical reads at the
// new speed). When the budget runs out the read with the fewest C2 errors
// is kept and the sector is recorded as unresolved (suspicious positions).
struct Ripper::C2Reread {
    std::vector<uint8_t> best, bestC2, prev;
    size_t bestBits = 0;
    size_t prevBits = 0;
    bool prevValid = false;
};

void Ripper::rereadC2Sector(uint32_t lba, uint8_t* out, const uint8_t* c2, TrackRipResult& result) {
    C2Reread state;
    state.best.assign(out, out + kSectorBytes);
    state.bestC2.assign(c2, c2 + kC2BytesPerSector);
    state.bestBits = countBits(c2);
    state.prev = state.best;
    state.prevBits = state.bestBits;
    state.prevValid = !options_.verify;
    if (rereadC2Loop(lba, out, state, options_.maxRetries, result)) return;

    if (slowdownActive_ && c2Active_ && options_.maxRetries > 0) {
        bool tried = false;
        for (int factor : currentSlowdownSteps()) {
            if (!setSpeed(speedKBps(factor), lba)) break;
            slowedKBps_ = speedKBps(factor);
            tried = true;
            state.prevValid = false;
            if (rereadC2Loop(lba, out, state, std::min(options_.maxRetries, 2), result)) {
                ++result.slowdownRecovered[factor];
                return;
            }
            if (!c2Active_) break;
        }
        if (tried) ++result.slowdownFailed;
    }
    std::memcpy(out, state.best.data(), kSectorBytes);
    ++result.c2Unresolved;
    unresolved_.push_back({lba, std::move(state.bestC2)});
}

// Up to `budget` re-reads of a C2 sector; true (and the sector in `out`)
// when one of the ends above was reached.
bool Ripper::rereadC2Loop(uint32_t lba, uint8_t* out, C2Reread& st, int budget, TrackRipResult& result) {
    std::vector<uint8_t> cur(kSectorBytes);
    std::vector<uint8_t> curC2(kC2BytesPerSector);
    for (int k = 0; k < budget && c2Active_; ++k) {
        ++result.c2Rereads;
        if (!readOnce(lba, 1, cur.data(), curC2.data(), true, result)) {
            st.prevValid = false;
            continue;
        }
        if (!c2Active_) break;  // fell back to plain reads: this one has no C2 bits
        const size_t bits = countBits(curC2.data());
        if (bits < st.bestBits) {
            st.best = cur;
            st.bestC2 = curC2;
            st.bestBits = bits;
        }
        const bool same = st.prevValid && cur == st.prev;
        if ((bits == 0 && !options_.verify) || same) {
            std::memcpy(out, cur.data(), kSectorBytes);
            if (bits == 0 || st.prevBits == 0) ++result.c2Recovered;
            else ++result.c2Matched;
            return true;
        }
        st.prev.swap(cur);
        st.prevBits = bits;
        st.prevValid = true;
    }
    return false;
}

}  // namespace cdr
