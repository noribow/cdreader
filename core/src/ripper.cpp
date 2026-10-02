#include "cdreader/ripper.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "cdreader/crc32.h"

namespace cdr {

TrackRipResult Ripper::ripTrack(const Track& track, const SampleSink& sink, const Progress& progress) {
    TrackRipResult result;
    result.track = track.number;
    result.sectors = track.lengthSectors;

    Crc32 crc;
    std::vector<uint8_t> buffer(size_t(kMaxSectorsPerRead) * kSectorBytes);
    uint32_t done = 0;
    if (progress) progress(0, track.lengthSectors);
    while (done < track.lengthSectors) {
        const uint32_t count = std::min(kMaxSectorsPerRead, track.lengthSectors - done);
        const size_t bytes = size_t(count) * kSectorBytes;
        readRange(track.startLba + done, count, buffer.data(), result);
        crc.update(buffer.data(), bytes);
        sink(buffer.data(), bytes);
        done += count;
        if (progress) progress(done, track.lengthSectors);
    }
    result.crc32 = crc.value();
    return result;
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
