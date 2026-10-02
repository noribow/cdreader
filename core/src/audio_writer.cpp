#include "cdreader/audio_writer.h"

#include "cdreader/flac_writer.h"
#include "cdreader/wav_writer.h"

namespace cdr {

std::vector<std::string> audioFormats() { return {"wav", "flac"}; }

std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format) {
    if (format == "wav") return std::make_unique<WavWriter>();
    if (format == "flac") return std::make_unique<FlacWriter>();
    return nullptr;
}

}  // namespace cdr
