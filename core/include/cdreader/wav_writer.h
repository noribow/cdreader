#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace cdr {

// Writes 44.1 kHz / 16-bit / stereo PCM (the CD-DA format) as a RIFF WAVE file.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    void open(const std::filesystem::path& path);  // throws std::runtime_error
    void write(const uint8_t* pcm, size_t bytes);
    void close();                                  // patches the RIFF sizes

    uint64_t dataBytes() const { return dataBytes_; }

private:
    void writeHeader(uint32_t dataBytes);

    std::ofstream out_;
    uint64_t dataBytes_ = 0;
};

}  // namespace cdr
