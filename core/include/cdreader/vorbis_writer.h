#pragma once

// Only available when built with CDREADER_WITH_VORBIS (CDREADER_HAVE_VORBIS
// is then defined for users of cdreader_core).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/ogg.h"

namespace cdr {

namespace vorbis {

constexpr double kDefaultQuality = 5;  // oggenc scale, about 160 kbit/s for CD audio
constexpr double kMinQuality = -1;
constexpr double kMaxQuality = 10;
constexpr int kMinBitrateKbps = 45;
constexpr int kMaxBitrateKbps = 500;

// "Xiph.Org libVorbis 1.3.7"
std::string libraryVersion();

// The Vorbis encoding without a container (shared by VorbisWriter for Ogg
// Vorbis and MkaWriter for Vorbis in Matroska): CD-DA PCM in, the three
// header packets and the audio packets out, from libvorbisenc at 44.1 kHz.
class PacketEncoder {
public:
    struct Headers {
        std::vector<uint8_t> identification, comment, setup;
    };
    struct Packet {
        const uint8_t* data;
        size_t size;
        int64_t granule;      // libvorbis' granule position: the end of the output, trimmed for the last packet
        bool last;            // end of stream
        uint64_t start;       // first output sample (the previous packet's granule position)
        uint64_t naturalEnd;  // start + the samples a decoder produces without end trimming
    };
    using PacketSink = std::function<void(const Packet&)>;

    // Settings as in VorbisWriter (checked there): a bitrate replaces the quality.
    PacketEncoder(double quality, std::optional<int> bitrateKbps);
    ~PacketEncoder();
    PacketEncoder(const PacketEncoder&) = delete;
    PacketEncoder& operator=(const PacketEncoder&) = delete;

    // Sets up the encoder and returns the header packets; the comment header
    // carries the tags of `metadata` and libvorbis' vendor string.
    Headers start(const TrackMetadata& metadata);
    void write(const uint8_t* pcm, size_t bytes, const PacketSink& sink);
    // Ends the input; the last packet is emitted with `last`. Throws when the
    // input ended in the middle of a sample.
    void finish(const PacketSink& sink);
    void release();

    bool started() const { return state_ != nullptr; }
    bool hasPartialSample() const { return !partial_.empty(); }
    uint64_t samples() const { return samples_; }

private:
    struct State;  // libvorbis structures
    void drain(const PacketSink& sink);

    double quality_;
    std::optional<int> bitrateKbps_;
    std::unique_ptr<State> state_;
    std::vector<uint8_t> partial_;  // bytes of an incomplete input sample
    uint64_t samples_ = 0;
    uint64_t lastGranule_ = 0;
    long lastBlockSize_ = 0;  // 0 before the first audio packet
};

}  // namespace vorbis

// Writes CD-DA PCM as an Ogg Vorbis file (extension .ogg) with libvorbisenc,
// at 44.1 kHz. VBR at a quality on the oggenc scale (-1..10), or, with a
// bitrate, VBR aiming at that average bitrate (like `oggenc -b`, without
// hard bitrate limits). The comment header carries the tags from
// TrackMetadata; the pages are written by OggStreamWriter, with the granule
// positions and the end of stream from libvorbis (the last granule position
// is the exact number of input samples).
class VorbisWriter : public AudioWriter {
public:
    // Throws std::invalid_argument for out-of-range settings.
    explicit VorbisWriter(std::optional<double> quality = std::nullopt, std::optional<int> bitrateKbps = std::nullopt);
    ~VorbisWriter() override;
    VorbisWriter(const VorbisWriter&) = delete;
    VorbisWriter& operator=(const VorbisWriter&) = delete;

    std::string extension() const override { return "ogg"; }
    std::string encoderDescription() const override;
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;

    uint64_t samples() const { return encoder_ ? encoder_->samples() : 0; }

private:
    void writePacket(const vorbis::PacketEncoder::Packet& packet);
    void writeBytes(const uint8_t* data, size_t size);

    double quality_ = vorbis::kDefaultQuality;
    std::optional<int> bitrateKbps_;
    std::unique_ptr<vorbis::PacketEncoder> encoder_;
    std::ofstream out_;
    std::unique_ptr<OggStreamWriter> ogg_;
};

}  // namespace cdr
