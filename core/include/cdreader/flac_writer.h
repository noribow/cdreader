#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/flac_encoder.h"
#include "cdreader/md5.h"

namespace cdr {

// Writes 44.1 kHz / 16-bit / stereo PCM as a native FLAC file (lossless).
// Metadata: STREAMINFO (with the MD5 of the audio), VORBIS_COMMENT tags from
// TrackMetadata, CUESHEET for disc images (see setEmbeddedCueSheet()),
// SEEKTABLE and PADDING.
class FlacWriter : public AudioWriter {
public:
    explicit FlacWriter(flac::EncoderOptions options = {});
    ~FlacWriter() override;
    FlacWriter(const FlacWriter&) = delete;
    FlacWriter& operator=(const FlacWriter&) = delete;

    std::string extension() const override { return "flac"; }
    std::string encoderDescription() const override { return "FLAC (built-in encoder), lossless"; }

    // A disc image carries its CUE sheet twice: as the native CUESHEET block
    // (read by libFLAC-based software, `metaflac --export-cuesheet-to`) and as
    // a CUESHEET tag holding the text (foobar2000 and others). The lead-out
    // is set to the actual length of the audio when the file is closed.
    bool canEmbedCueSheet() const override { return true; }
    void setEmbeddedCueSheet(const EmbeddedCueSheet& cue) override { cue_ = cue; }
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;  // encodes the last block, patches STREAMINFO and SEEKTABLE

    uint64_t totalSamples() const { return totalSamples_; }

private:
    void encodeBlock(const uint8_t* pcm, unsigned samples);
    void writeBytes(const uint8_t* data, size_t size);

    flac::FrameEncoder encoder_;
    std::ofstream out_;
    std::vector<uint8_t> pending_;  // PCM bytes not yet encoded (less than one block)
    std::vector<int32_t> left_, right_;
    std::vector<uint64_t> frameOffsets_;  // byte offset of each frame from the first one
    Md5 md5_;
    uint64_t totalSamples_ = 0;
    uint64_t audioBytes_ = 0;  // encoded frame bytes
    uint32_t minFrameBytes_ = 0, maxFrameBytes_ = 0;
    uint64_t reservedOffset_ = 0;  // file offset of the SEEKTABLE + PADDING area
    std::optional<EmbeddedCueSheet> cue_;
    uint64_t leadOutOffsetPos_ = 0;  // file offset of the CUESHEET lead-out offset (0: no CUESHEET)
};

}  // namespace cdr
