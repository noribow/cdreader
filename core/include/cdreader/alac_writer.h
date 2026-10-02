#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "cdreader/alac_encoder.h"
#include "cdreader/audio_writer.h"

namespace cdr {

// Writes 44.1 kHz / 16-bit / stereo PCM as Apple Lossless (ALAC) in an MP4
// file (.m4a, lossless). The frames are streamed into "mdat"; the "moov" box
// (sample tables, the ALAC magic cookie, iTunes tags from TrackMetadata) is
// appended when the file is closed. See mp4.h for the layout.
class AlacWriter : public AudioWriter {
public:
    explicit AlacWriter(alac::EncoderOptions options = {});
    ~AlacWriter() override;
    AlacWriter(const AlacWriter&) = delete;
    AlacWriter& operator=(const AlacWriter&) = delete;

    std::string extension() const override { return "m4a"; }
    std::string encoderDescription() const override { return "ALAC (built-in encoder), lossless, MP4"; }
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;  // encodes the last frame, patches the mdat size, appends moov

    uint64_t totalSamples() const { return totalSamples_; }
    unsigned escapedFrames() const { return escapedFrames_; }  // stored uncompressed

private:
    void encodeFrame(const uint8_t* pcm, unsigned samples);
    void writeBytes(const uint8_t* data, size_t size);

    alac::EncoderOptions options_;
    alac::FrameEncoder encoder_;
    std::ofstream out_;
    TrackMetadata metadata_;
    std::vector<uint8_t> pending_;  // PCM bytes not yet encoded (less than one frame)
    std::vector<int32_t> left_, right_;
    std::vector<uint32_t> frameSizes_;
    uint64_t mdatOffset_ = 0;  // file offset of the 16-byte area holding the mdat header
    uint64_t position_ = 0;    // current file size
    uint64_t totalSamples_ = 0;
    unsigned escapedFrames_ = 0;
};

}  // namespace cdr
