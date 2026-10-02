#pragma once

// Minimal FLAC decoder for the tests: decodes what FlacWriter produces
// (fixed block size, 16-bit stereo) and checks CRCs strictly. Independent
// of the encoder except for the shared CRC functions.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct DecodedFlac {
    unsigned minBlockSize = 0, maxBlockSize = 0;
    uint32_t minFrameSize = 0, maxFrameSize = 0;
    uint32_t sampleRate = 0;
    unsigned channels = 0, bitsPerSample = 0;
    uint64_t totalSamples = 0;
    std::array<uint8_t, 16> md5{};
    std::string vendor;
    std::vector<std::string> comments;
    struct SeekPoint {
        uint64_t sample, offset;
        unsigned samples;
    };
    std::vector<SeekPoint> seekPoints;
    std::vector<int> blockTypes;  // metadata block types in file order
    unsigned frames = 0;
    size_t firstFrame = 0;                // file offset of the first frame
    std::vector<uint64_t> frameOffsets;  // relative to firstFrame
    std::vector<uint8_t> pcm;  // interleaved 16-bit little-endian
};

// Throws std::runtime_error on malformed input.
DecodedFlac decodeFlac(const std::vector<uint8_t>& file);
