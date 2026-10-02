#include "cdreader/wav_writer.h"

#include <stdexcept>

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

void WavWriter::open(const std::filesystem::path& path) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    dataBytes_ = 0;
    writeHeader(0);
}

void WavWriter::write(const uint8_t* pcm, size_t bytes) {
    out_.write(reinterpret_cast<const char*>(pcm), std::streamsize(bytes));
    if (!out_) throw std::runtime_error("write error while saving WAV data");
    dataBytes_ += bytes;
}

void WavWriter::close() {
    if (!out_.is_open()) return;
    if (dataBytes_ > 0xFFFFFFFFu - kHeaderBytes) throw std::runtime_error("WAV data exceeds 4 GiB");
    out_.seekp(0);
    writeHeader(uint32_t(dataBytes_));
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize WAV file");
}

void WavWriter::writeHeader(uint32_t dataBytes) {
    uint8_t h[kHeaderBytes] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                               'f', 'm', 't', ' ', 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               'd', 'a', 't', 'a', 0, 0, 0, 0};
    put32(h + 4, kHeaderBytes - 8 + dataBytes);
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
