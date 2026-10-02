#pragma once

// Only available when built with CDREADER_WITH_OPUS (CDREADER_HAVE_OPUS is
// then defined for users of cdreader_core).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/ogg.h"
#include "cdreader/resampler.h"

struct OpusEncoder;  // libopus

namespace cdr {

namespace opus {

constexpr uint32_t kSampleRate = 48000;     // Opus always runs at 48 kHz
constexpr unsigned kFrameSamples = 960;     // 20 ms packets
constexpr int kDefaultBitrateKbps = 160;
constexpr int kMinBitrateKbps = 6;
constexpr int kMaxBitrateKbps = 510;

// Identification header (RFC 7845 section 5.1): "OpusHead", version 1,
// channel count, pre-skip, original input sample rate, output gain 0,
// channel mapping family 0.
std::vector<uint8_t> headPacket(uint8_t channels, uint16_t preSkip, uint32_t inputSampleRate);

// Comment header (RFC 7845 section 5.2): "OpusTags" followed by a Vorbis
// comment structure (see vorbisComment() in tags.h).
std::vector<uint8_t> tagsPacket(const TrackMetadata& metadata, const std::string& vendor);

// "libopus 1.5.2"
std::string libraryVersion();

}  // namespace opus

// Writes CD-DA PCM as an Ogg Opus file (RFC 7845, extension .opus).
//
// The 44.1 kHz input is resampled to 48 kHz (Resampler) and encoded in 20 ms
// packets with libopus (VBR, the "audio" application). Granule positions
// count 48 kHz samples including the pre-skip (the encoder's look-ahead); the
// last page carries the EOS flag and a granule position of exactly
// pre-skip + the resampled length, so that decoders trim the padding of the
// last packet and return the original duration. Tags come from TrackMetadata.
class OpusWriter : public AudioWriter {
public:
    explicit OpusWriter(int bitrateKbps = opus::kDefaultBitrateKbps);  // throws std::invalid_argument
    ~OpusWriter() override;
    OpusWriter(const OpusWriter&) = delete;
    OpusWriter& operator=(const OpusWriter&) = delete;

    std::string extension() const override { return "opus"; }
    std::string encoderDescription() const override;
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;

    int bitrateKbps() const { return bitrateKbps_; }
    uint16_t preSkip() const { return preSkip_; }
    uint64_t inputSamples() const { return resampler_ ? resampler_->inputFrames() : 0; }  // 44.1 kHz
    uint64_t finalGranulePosition() const { return finalGranule_; }

private:
    void encodeAvailable(bool final);
    void emitPacket(const uint8_t* data, size_t size, bool last);
    void writeBytes(const uint8_t* data, size_t size);
    void release();

    int bitrateKbps_;
    OpusEncoder* encoder_ = nullptr;
    uint16_t preSkip_ = 0;
    std::ofstream out_;
    std::unique_ptr<OggStreamWriter> ogg_;
    std::unique_ptr<Resampler> resampler_;
    std::vector<uint8_t> partial_;     // bytes of an incomplete input sample
    std::vector<float> input_;         // converted input for the resampler
    std::vector<float> pending_;       // 48 kHz samples not yet encoded (interleaved)
    std::vector<uint8_t> packet_;      // encoded packet waiting to be written
    bool havePacket_ = false;
    uint64_t encodedSamples_ = 0;      // 48 kHz samples given to the encoder (padding included)
    uint64_t finalGranule_ = 0;
};

}  // namespace cdr
