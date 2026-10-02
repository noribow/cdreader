#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/matroska.h"

namespace cdr {

// Writes CD-DA PCM as a Matroska audio file (.mka, #23) with one of these codecs:
//
//   Flac    A_FLAC         built-in FLAC encoder (lossless); CodecPrivate =
//                          "fLaC" + STREAMINFO (completed when the file is closed)
//   Pcm     A_PCM/INT/LIT  16-bit little-endian PCM as read from the disc, 100 ms per block
//   Opus    A_OPUS         libopus (as OpusWriter); CodecPrivate = OpusHead,
//                          CodecDelay = pre-skip, SeekPreRoll 80 ms, the last
//                          block carries DiscardPadding (end trimming)
//   Vorbis  A_VORBIS       libvorbis (as VorbisWriter); CodecPrivate = the
//                          three headers, Xiph laced; DiscardPadding on the last block
//
// Opus / Vorbis are only available when built in (codecAvailable()).
// Tags (Matroska Tags, RFC 9559 section 5.1.8):
//   level 50 (ALBUM): TITLE and ALBUM = album, ARTIST = album artist,
//                     TOTAL_PARTS, DATE_RELEASED, GENRE, CDDB (disc id)
//   level 30 (TRACK): TITLE, ARTIST, PART_NUMBER (per-track files)
// Disc images (setEmbeddedCueSheet()) get a chapter per CUE track instead of
// the level 30 tag, with a level 30 tag per chapter (TITLE, ARTIST,
// PART_NUMBER).
class MkaWriter : public AudioWriter {
public:
    enum class Codec { Flac, Pcm, Opus, Vorbis };

    static bool codecAvailable(Codec codec);

    // Throws std::invalid_argument for an unavailable codec or settings it
    // does not take / out of range (the same rules as the Ogg writers).
    explicit MkaWriter(Codec codec, const EncoderSettings& settings = {});
    ~MkaWriter() override;
    MkaWriter(const MkaWriter&) = delete;
    MkaWriter& operator=(const MkaWriter&) = delete;

    std::string extension() const override { return "mka"; }
    std::string encoderDescription() const override;
    bool canEmbedCueSheet() const override { return true; }
    void setEmbeddedCueSheet(const EmbeddedCueSheet& cue) override { cue_ = cue; }
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;

    Codec codec() const { return codec_; }

    // Chapters of a disc image: one per CUE track, from its INDEX 01 to the
    // next track (the last one to the lead-out), times rounded to nanoseconds.
    static std::vector<mkv::Chapter> chaptersFor(const EmbeddedCueSheet& cue, const TrackMetadata& album);
    // The Tags written for `metadata` (and the chapters of a disc image).
    static std::vector<mkv::Tag> tagsFor(const TrackMetadata& metadata, const std::vector<mkv::Chapter>& chapters,
                                         const EmbeddedCueSheet* cue);

    struct Encoder;  // the codec behind the Matroska track (mka_writer.cpp)

private:
    Codec codec_;
    std::unique_ptr<Encoder> encoder_;
    std::optional<EmbeddedCueSheet> cue_;
    std::unique_ptr<std::ofstream> out_;
    std::unique_ptr<mkv::Muxer> muxer_;
};

}  // namespace cdr
