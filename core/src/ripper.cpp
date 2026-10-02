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
    C2Availability a;
    if (!wanted) return a;
    const DriveCapabilities caps = drive.readCapabilities();
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

    const Toc::LbaRange readable = toc_.audioRange(track);
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
        readSpan(firstSector + sectorsDone, count, readable, buffer.data(), result);
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
    return result;
}

// Fills `count` sectors starting at `lba` (which may be negative): sectors
// inside `readable` come from the drive, the rest are silence.
void Ripper::readSpan(int64_t lba, uint32_t count, const Toc::LbaRange& readable, uint8_t* out,
                      TrackRipResult& result) {
    const int64_t begin = std::max<int64_t>(lba, readable.begin);
    const int64_t end = std::min<int64_t>(lba + count, readable.end);
    std::memset(out, 0, size_t(count) * kSectorBytes);
    if (begin < end)
        readRange(uint32_t(begin), uint32_t(end - begin), out + size_t(begin - lba) * kSectorBytes, result);
}

// One READ CD of `count` sectors into `out`, with the C2 bits into `c2`
// (count * kC2BytesPerSector bytes, all zero when C2 pointers are not used).
// A drive that rejects the C2 read (ILLEGAL REQUEST, or less data) while a
// plain read of the same sectors works cannot deliver C2 bits: plain reads
// from then on, for the rest of the disc.
bool Ripper::readOnce(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2) {
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
bool Ripper::readBlock(uint32_t lba, uint32_t count, uint8_t* out, uint8_t* c2, TrackRipResult& result) {
    const size_t bytes = size_t(count) * kSectorBytes;
    std::vector<uint8_t> check, checkC2;
    if (options_.verify) {
        check.resize(bytes);
        checkC2.resize(size_t(count) * kC2BytesPerSector);
    }

    for (int attempt = 0; attempt <= options_.maxRetries; ++attempt) {
        if (attempt > 0) ++result.retries;
        if (!readOnce(lba, count, out, c2)) continue;
        if (!options_.verify) return true;
        if (!readOnce(lba, count, check.data(), checkC2.data())) continue;
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
void Ripper::readRange(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result) {
    std::vector<uint8_t> c2(size_t(count) * kC2BytesPerSector);
    if (readBlock(lba, count, out, c2.data(), result)) {
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
    for (uint32_t i = 0; i < count; ++i) readRange(lba + i, 1, out + size_t(i) * kSectorBytes, result);
}

// Re-reads a sector the drive flagged with C2 errors, up to maxRetries
// times. It ends with
// - a read without C2 errors (verify mode: confirmed by an identical read), or
// - two consecutive identical reads that still have C2 errors: the drive
//   returns the same data every time (or reports false C2 errors), so more
//   reads are unlikely to change anything. In verify mode both must be
//   re-reads; otherwise the read that flagged the sector counts as well.
// When the budget runs out the read with the fewest C2 errors is kept and
// the sector is recorded as unresolved (suspicious positions).
void Ripper::rereadC2Sector(uint32_t lba, uint8_t* out, const uint8_t* c2, TrackRipResult& result) {
    std::vector<uint8_t> best(out, out + kSectorBytes);
    std::vector<uint8_t> bestC2(c2, c2 + kC2BytesPerSector);
    size_t bestBits = countBits(c2);
    std::vector<uint8_t> prev = best;
    size_t prevBits = bestBits;
    bool prevValid = !options_.verify;
    std::vector<uint8_t> cur(kSectorBytes);
    std::vector<uint8_t> curC2(kC2BytesPerSector);

    for (int k = 0; k < options_.maxRetries && c2Active_; ++k) {
        ++result.c2Rereads;
        if (!readOnce(lba, 1, cur.data(), curC2.data())) {
            prevValid = false;
            continue;
        }
        if (!c2Active_) break;  // fell back to plain reads: this one has no C2 bits
        const size_t bits = countBits(curC2.data());
        if (bits < bestBits) {
            best = cur;
            bestC2 = curC2;
            bestBits = bits;
        }
        const bool same = prevValid && cur == prev;
        if ((bits == 0 && !options_.verify) || same) {
            std::memcpy(out, cur.data(), kSectorBytes);
            if (bits == 0 || prevBits == 0) ++result.c2Recovered;
            else ++result.c2Matched;
            return;
        }
        prev.swap(cur);
        prevBits = bits;
        prevValid = true;
    }
    std::memcpy(out, best.data(), kSectorBytes);
    ++result.c2Unresolved;
    unresolved_.push_back({lba, std::move(bestC2)});
}

}  // namespace cdr
