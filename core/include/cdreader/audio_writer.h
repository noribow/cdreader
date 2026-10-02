#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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

    virtual void open(const std::filesystem::path& path, const TrackMetadata& metadata) = 0;
    virtual void write(const uint8_t* pcm, size_t bytes) = 0;
    virtual void close() = 0;  // finalizes the file; throws std::runtime_error on failure
};

// Names accepted by createAudioWriter(), e.g. {"wav"}.
std::vector<std::string> audioFormats();

// Returns nullptr for an unknown format name.
std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format);

}  // namespace cdr
