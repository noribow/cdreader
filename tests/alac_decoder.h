#pragma once

// Minimal MP4 reader and ALAC decoder for the tests: walks the box tree of
// what AlacWriter produces (checking that every box lies exactly within its
// parent), reads the sample tables, the ALAC magic cookie and the iTunes
// tags, and decodes every frame. Independent of the encoder: the bitstream
// is read as Apple's reference decoder (github.com/macosforge/alac) does.
// Throws std::runtime_error on malformed input.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct Mp4Box {
    std::string type;
    uint64_t offset = 0;      // of the box header in the file
    uint64_t size = 0;        // whole box
    unsigned headerSize = 0;  // 8, or 16 with a 64-bit size
    std::vector<Mp4Box> children;

    const Mp4Box* child(const std::string& t) const;
    // Descendant along a path like "moov/trak/mdia"; nullptr when missing.
    const Mp4Box* find(const std::string& path) const;
};

struct AlacFrameInfo {
    bool partial = false, escaped = false;
    unsigned samples = 0;
    unsigned mixBits = 0, mixRes = 0;
    unsigned modeU = 0, modeV = 0, denShiftU = 0, denShiftV = 0, pbFactorU = 0, pbFactorV = 0;
    unsigned orderU = 0, orderV = 0;
};

struct DecodedM4a {
    Mp4Box root;  // type "" spanning the file; children are the top-level boxes
    std::string majorBrand;
    std::vector<std::string> compatibleBrands;

    uint32_t movieTimescale = 0, mediaTimescale = 0;
    uint64_t movieDuration = 0, trackDuration = 0, mediaDuration = 0;
    std::string handlerType;

    // Sample entry and magic cookie (ALACSpecificConfig, big-endian fields).
    std::string sampleEntryType;
    unsigned entryChannels = 0, entrySampleSize = 0;
    uint32_t entrySampleRate = 0;
    std::vector<uint8_t> cookie;  // the 24 bytes
    uint32_t frameLength = 0, maxFrameBytes = 0, avgBitRate = 0, sampleRate = 0;
    unsigned compatibleVersion = 0, bitDepth = 0, pb = 0, mb = 0, kb = 0, channels = 0, maxRun = 0;

    std::vector<std::pair<uint32_t, uint32_t>> stts;                // (count, duration)
    std::vector<std::array<uint32_t, 3>> stsc;                      // (first chunk, samples per chunk, description)
    std::vector<uint32_t> sampleSizes;
    uint32_t stszSampleSize = 0;  // the constant size, 0 when stsz lists every frame
    std::vector<uint64_t> chunkOffsets;
    bool co64 = false;
    std::vector<uint64_t> frameOffsets;  // resolved from stsc / stco / stsz
    uint64_t mdatDataOffset = 0, mdatDataSize = 0;

    // ilst items: text items by type ("\xA9nam", ...), "trkn" as "n/total",
    // freeform items as "----:<mean>:<name>".
    std::map<std::string, std::string> tags;
    std::string metaHandler;

    std::vector<AlacFrameInfo> frames;
    std::vector<uint8_t> pcm;  // interleaved 16-bit little-endian
};

DecodedM4a decodeM4a(const std::vector<uint8_t>& file);

// Decodes one ALAC frame of a 16-bit stereo stream with frame length
// `frameLength` (and the cookie's pb / mb / kb), appending the samples.
AlacFrameInfo decodeAlacFrame(const uint8_t* data, size_t size, uint32_t frameLength, unsigned pb, unsigned mb,
                              unsigned kb, std::vector<uint8_t>& pcm);
