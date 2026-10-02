#include "cdreader/wav_writer.h"

#include <stdexcept>
#include <vector>

#include "cdreader/tags.h"

namespace cdr {

namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint16_t kChannels = 2;
constexpr uint16_t kBitsPerSample = 16;
constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;
constexpr uint32_t kHeaderBytes = 44;

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

}  // namespace

WavWriter::~WavWriter() {
    try {
        close();
    } catch (...) {
    }
}

void WavWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    dataBytes_ = 0;
    metadata_ = metadata;
    writeHeader(kHeaderBytes - 8, 0);
}

void WavWriter::write(const uint8_t* pcm, size_t bytes) {
    out_.write(reinterpret_cast<const char*>(pcm), std::streamsize(bytes));
    if (!out_) throw std::runtime_error("write error while saving WAV data");
    dataBytes_ += bytes;
}

// Layout: RIFF/WAVE, fmt, data, [LIST/INFO], ["id3 "].
// The tags go after the audio so that the header keeps the canonical 44-byte
// form (simple readers that assume the samples start at offset 44 keep
// working) and so that audio can be streamed without knowing the tag sizes
// up front. RIFF allows chunks in any order, and tag readers such as ffmpeg,
// MediaInfo and ExifTool scan past the data chunk (checked with all three).
// Many players ignore INFO and read only ID3, hence both.
void WavWriter::close() {
    if (!out_.is_open()) return;
    std::vector<uint8_t> tail;
    if (dataBytes_ % 2) tail.push_back(0);  // chunk pad byte (not counted in the data size)
    const std::vector<uint8_t> info = riffInfoChunk(metadata_);
    tail.insert(tail.end(), info.begin(), info.end());
    const std::vector<uint8_t> id3 = id3v2Tag(metadata_);
    if (!id3.empty()) {
        uint8_t h[8] = {'i', 'd', '3', ' '};
        put32(h + 4, uint32_t(id3.size()));
        tail.insert(tail.end(), h, h + 8);
        tail.insert(tail.end(), id3.begin(), id3.end());
        if (id3.size() % 2) tail.push_back(0);
    }

    const uint64_t riffBytes = kHeaderBytes - 8 + dataBytes_ + tail.size();
    try {
        if (riffBytes > 0xFFFFFFFFu) throw std::runtime_error("WAV data exceeds 4 GiB");
        out_.write(reinterpret_cast<const char*>(tail.data()), std::streamsize(tail.size()));
        out_.seekp(0);
        writeHeader(uint32_t(riffBytes), uint32_t(dataBytes_));
    } catch (...) {
        out_.close();  // never append the tags twice (the destructor calls close() again)
        throw;
    }
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize WAV file");
}

void WavWriter::writeHeader(uint32_t riffBytes, uint32_t dataBytes) {
    uint8_t h[kHeaderBytes] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                               'f', 'm', 't', ' ', 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               'd', 'a', 't', 'a', 0, 0, 0, 0};
    put32(h + 4, riffBytes);
    put32(h + 16, 16);  // fmt chunk size
    put16(h + 20, 1);   // PCM
    put16(h + 22, kChannels);
    put32(h + 24, kSampleRate);
    put32(h + 28, kSampleRate * kBlockAlign);
    put16(h + 32, kBlockAlign);
    put16(h + 34, kBitsPerSample);
    put32(h + 40, dataBytes);
    out_.write(reinterpret_cast<const char*>(h), sizeof h);
    if (!out_) throw std::runtime_error("failed to write WAV header");
}

}  // namespace cdr
