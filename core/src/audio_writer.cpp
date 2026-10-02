#include "cdreader/audio_writer.h"

#include <stdexcept>

#include "cdreader/flac_writer.h"
#include "cdreader/mka_writer.h"
#include "cdreader/ogg_flac_writer.h"
#include "cdreader/wav_writer.h"
#ifdef CDREADER_HAVE_OPUS
#include "cdreader/opus_writer.h"
#endif
#ifdef CDREADER_HAVE_VORBIS
#include "cdreader/vorbis_writer.h"
#endif

namespace cdr {

std::vector<std::string> audioFormats() {
    std::vector<std::string> formats = {"wav", "flac", "oggflac"};
#ifdef CDREADER_HAVE_OPUS
    formats.push_back("opus");
#endif
#ifdef CDREADER_HAVE_VORBIS
    formats.push_back("vorbis");
#endif
    // Matroska (#23): "mka" holds FLAC, the others name their codec.
    formats.push_back("mka");
    formats.push_back("mka-pcm");
    if (MkaWriter::codecAvailable(MkaWriter::Codec::Opus)) formats.push_back("mka-opus");
    if (MkaWriter::codecAvailable(MkaWriter::Codec::Vorbis)) formats.push_back("mka-vorbis");
    return formats;
}

bool isLossyFormat(const std::string& format) {
    return format == "opus" || format == "vorbis" || format == "mka-opus" || format == "mka-vorbis";
}

std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format, const EncoderSettings& settings) {
    if (format == "wav" || format == "flac" || format == "oggflac") {
        if (!settings.empty())
            throw std::invalid_argument(format + " is lossless: a bitrate or quality does not apply");
        if (format == "wav") return std::make_unique<WavWriter>();
        if (format == "oggflac") return std::make_unique<OggFlacWriter>();
        return std::make_unique<FlacWriter>();
    }
#ifdef CDREADER_HAVE_OPUS
    if (format == "opus") {
        if (settings.quality) throw std::invalid_argument("Opus takes a bitrate, not a quality");
        return std::make_unique<OpusWriter>(settings.bitrateKbps.value_or(opus::kDefaultBitrateKbps));
    }
#endif
#ifdef CDREADER_HAVE_VORBIS
    if (format == "vorbis") return std::make_unique<VorbisWriter>(settings.quality, settings.bitrateKbps);
#endif
    if (format == "mka" || format == "mka-flac") return std::make_unique<MkaWriter>(MkaWriter::Codec::Flac, settings);
    if (format == "mka-pcm") return std::make_unique<MkaWriter>(MkaWriter::Codec::Pcm, settings);
    if (format == "mka-opus" && MkaWriter::codecAvailable(MkaWriter::Codec::Opus))
        return std::make_unique<MkaWriter>(MkaWriter::Codec::Opus, settings);
    if (format == "mka-vorbis" && MkaWriter::codecAvailable(MkaWriter::Codec::Vorbis))
        return std::make_unique<MkaWriter>(MkaWriter::Codec::Vorbis, settings);
    return nullptr;
}

}  // namespace cdr
