#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "cdreader/audio_writer.h"

namespace cdr {

// Writes 44.1 kHz / 16-bit / stereo PCM (the CD-DA format) as a RIFF WAVE file.
class WavWriter : public AudioWriter {
public:
    WavWriter() = default;
    ~WavWriter() override;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    std::string extension() const override { return "wav"; }
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void open(const std::filesystem::path& path) { open(path, TrackMetadata{}); }  // throws std::runtime_error
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;                         // patches the RIFF sizes

    uint64_t dataBytes() const { return dataBytes_; }

private:
    void writeHeader(uint32_t dataBytes);

    std::ofstream out_;
    uint64_t dataBytes_ = 0;
};

}  // namespace cdr
