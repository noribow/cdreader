#pragma once

#include <cstdint>
#include <functional>

#include "cdreader/cd_drive.h"
#include "cdreader/toc.h"

namespace cdr {

struct RipOptions {
    int maxRetries = 5;   // extra attempts per failing read
    bool verify = false;  // read every block twice and require identical data
};

struct TrackRipResult {
    int track = 0;
    uint32_t sectors = 0;
    uint32_t unreadableSectors = 0;  // replaced with silence
    uint32_t retries = 0;            // failed or mismatching reads that were retried
    uint32_t crc32 = 0;

    bool clean() const { return unreadableSectors == 0; }
};

class Ripper {
public:
    using SampleSink = std::function<void(const uint8_t* pcm, size_t bytes)>;
    using Progress = std::function<void(uint32_t doneSectors, uint32_t totalSectors)>;

    Ripper(CdDrive& drive, RipOptions options) : drive_(drive), options_(options) {}

    // Reads an audio track and streams its PCM data to `sink`.
    TrackRipResult ripTrack(const Track& track, const SampleSink& sink,
                            const Progress& progress = {});

private:
    bool readBlock(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result);
    void readRange(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result);

    CdDrive& drive_;
    RipOptions options_;
};

}  // namespace cdr
