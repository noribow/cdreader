#include "cdreader/audio_writer.h"

#include "cdreader/wav_writer.h"

namespace cdr {

std::vector<std::string> audioFormats() { return {"wav"}; }

std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format) {
    if (format == "wav") return std::make_unique<WavWriter>();
    return nullptr;
}

}  // namespace cdr
