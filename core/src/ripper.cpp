#include "cdreader/ripper.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "cdreader/crc32.h"

namespace cdr {

namespace {

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

}  // namespace

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

    Crc32 crc;
    std::vector<uint8_t> buffer(size_t(kMaxSectorsPerRead) * kSectorBytes);
    uint64_t emitted = 0;
    uint32_t sectorsDone = 0;
    if (progress) progress(0, track.lengthSectors);
    while (sectorsDone < sectorsToRead) {
        const uint32_t count = std::min(kMaxSectorsPerRead, sectorsToRead - sectorsDone);
        readSpan(firstSector + sectorsDone, count, readable, buffer.data(), result);
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

// Reads a block with retries; in verify mode a block only counts once two
// consecutive reads return identical data.
bool Ripper::readBlock(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result) {
    const size_t bytes = size_t(count) * kSectorBytes;
    std::vector<uint8_t> check;
    if (options_.verify) check.resize(bytes);

    for (int attempt = 0; attempt <= options_.maxRetries; ++attempt) {
        if (attempt > 0) ++result.retries;
        if (!drive_.readAudio(lba, count, out).ok()) continue;
        if (!options_.verify) return true;
        if (!drive_.readAudio(lba, count, check.data()).ok()) continue;
        if (std::memcmp(out, check.data(), bytes) == 0) return true;
    }
    return false;
}

// Falls back to sector-by-sector reads when a multi-sector block keeps
// failing, so that a single bad sector does not silence its neighbours.
void Ripper::readRange(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result) {
    if (readBlock(lba, count, out, result)) return;
    if (count == 1) {
        std::memset(out, 0, kSectorBytes);
        ++result.unreadableSectors;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) readRange(lba + i, 1, out + size_t(i) * kSectorBytes, result);
}

}  // namespace cdr
