#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/metadata.h"

namespace cdr {

// Output file for one track: receives CD-DA PCM (44.1 kHz, 16-bit, stereo,
// little-endian) and encodes / wraps it in a file format. New codecs (#7)
// and containers (#8) implement this interface and register in
// createAudioWriter().
class AudioWriter {
public:
    virtual ~AudioWriter() = default;

    // File name extension without the dot, e.g. "wav".
    virtual std::string extension() const = 0;

    // Whether the format can carry a CUE sheet inside the file (single-file
    // rips); if so, setEmbeddedCueSheet() must be called before open().
    virtual bool canEmbedCueSheet() const { return false; }
    virtual void setEmbeddedCueSheet(const EmbeddedCueSheet&) {}

    // Codec and settings in words for rip.log, e.g. "Opus (libopus 1.5.2),
    // VBR 160 kbps"; empty for formats without settings (WAV).
    virtual std::string encoderDescription() const { return {}; }

    virtual void open(const std::filesystem::path& path, const TrackMetadata& metadata) = 0;
    virtual void write(const uint8_t* pcm, size_t bytes) = 0;
    virtual void close() = 0;  // finalizes the file; throws std::runtime_error on failure
};

// Settings of the lossy encoders (#13). Unset values mean the format's default.
struct EncoderSettings {
    // Target bitrate in kbit/s (Opus: VBR, 6..510, default 160;
    // Vorbis: average bitrate, 45..500, instead of the quality).
    std::optional<int> bitrateKbps;
    // Vorbis VBR quality on the oggenc scale -1..10 (default 5, ~160 kbit/s).
    std::optional<double> quality;

    bool empty() const { return !bitrateKbps && !quality; }
};

// Names accepted by createAudioWriter(), e.g. {"wav", "flac", "opus", "vorbis",
// "mka", "mka-pcm", "mka-opus", "mka-vorbis"}. Matroska (.mka) formats name
// the codec inside: "mka" is FLAC ("mka-flac" is accepted as well).
// The lossy formats are only listed when the codec library was built in
// (CMake options CDREADER_WITH_OPUS / CDREADER_WITH_VORBIS).
std::vector<std::string> audioFormats();

// Whether `format` is a lossy format, i.e. takes EncoderSettings.
bool isLossyFormat(const std::string& format);

// Returns nullptr for an unknown format name. Throws std::invalid_argument
// for settings the format does not take or that are out of range (with a
// message for the user).
std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format, const EncoderSettings& settings = {});

}  // namespace cdr
