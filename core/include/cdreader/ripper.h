#pragma once

#include <cstdint>
#include <functional>

#include "cdreader/cd_drive.h"
#include "cdreader/toc.h"

namespace cdr {

struct RipOptions {
    int maxRetries = 5;   // extra attempts per failing read
    bool verify = false;  // read every block twice and require identical data

    // Drive read offset correction in samples (one sample = 4 bytes, stereo
    // 16-bit), using the same sign convention as EAC / AccurateRip: with +N
    // the ripper outputs the data the drive reports N samples later.
    int readOffsetSamples = 0;
};

struct TrackRipResult {
    int track = 0;
    uint32_t sectors = 0;
    uint32_t unreadableSectors = 0;  // replaced with silence
    uint32_t retries = 0;            // failed or mismatching reads that were retried
    uint32_t paddedSamples = 0;      // samples outside the readable area (offset correction), filled with silence
    uint32_t crc32 = 0;

    bool clean() const { return unreadableSectors == 0; }
};

class Ripper {
public:
    using SampleSink = std::function<void(const uint8_t* pcm, size_t bytes)>;
    using Progress = std::function<void(uint32_t doneSectors, uint32_t totalSectors)>;

    Ripper(CdDrive& drive, const Toc& toc, RipOptions options) : drive_(drive), toc_(toc), options_(options) {}

    // Reads an audio track and streams its offset-corrected PCM data to `sink`.
    TrackRipResult ripTrack(const Track& track, const SampleSink& sink,
                            const Progress& progress = {});

private:
    void readSpan(int64_t lba, uint32_t count, const Toc::LbaRange& readable, uint8_t* out,
                  TrackRipResult& result);
    bool readBlock(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result);
    void readRange(uint32_t lba, uint32_t count, uint8_t* out, TrackRipResult& result);

    CdDrive& drive_;
    const Toc& toc_;
    RipOptions options_;
};

}  // namespace cdr
