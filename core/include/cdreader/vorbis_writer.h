#pragma once

// Only available when built with CDREADER_WITH_VORBIS (CDREADER_HAVE_VORBIS
// is then defined for users of cdreader_core).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

    uint64_t samples() const { return samples_; }

private:
    struct State;  // libvorbis structures
    void drain();
    void writeBytes(const uint8_t* data, size_t size);

    double quality_ = vorbis::kDefaultQuality;
    std::optional<int> bitrateKbps_;
    std::unique_ptr<State> state_;
    std::ofstream out_;
    std::unique_ptr<OggStreamWriter> ogg_;
    std::vector<uint8_t> partial_;  // bytes of an incomplete input sample
    uint64_t samples_ = 0;
};

}  // namespace cdr
