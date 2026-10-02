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
#include "cdreader/flac_encoder.h"
#include "cdreader/ogg.h"

namespace cdr {

namespace oggflac {

// Size of the first packet of the FLAC-to-Ogg mapping: 0x7F "FLAC", mapping
// version 1.0, header packet count, "fLaC" and the STREAMINFO block.
constexpr size_t kFirstPacketBytes = 1 + 4 + 2 + 2 + 4 + 4 + flac::kStreamInfoBytes;  // 51

// First packet (https://xiph.org/flac/ogg_mapping.html). `headerPackets` is
// the number of header packets that follow it (the other metadata blocks);
// `streamInfo` is the 34-byte STREAMINFO body.
std::vector<uint8_t> firstPacket(uint16_t headerPackets, const std::vector<uint8_t>& streamInfo);

}  // namespace oggflac

// Writes CD-DA PCM as an Ogg FLAC file (extension .oga): the same lossless
// FLAC frames as FlacWriter, packed into Ogg pages per the FLAC-to-Ogg
// mapping 1.0.
//
// Packets: the first packet (STREAMINFO, alone on the BOS page), then one
// packet per further metadata block (VORBIS_COMMENT; CUESHEET for disc
// images), then one FLAC frame per packet. The header pages have granule
// position 0 and the audio starts on a new page; an audio page's granule
// position is the number of samples in the frames completed on it, the last
// page carries the EOS flag. No SEEKTABLE (Ogg is seeked by granule
// position) and no PADDING (tag editors rewrite Ogg files anyway).
//
// STREAMINFO (sample count, frame sizes, MD5) and the CUESHEET lead-out are
// only known at the end: close() rebuilds the header pages, which keep their
// size, and writes them over the placeholders at the start of the file.
class OggFlacWriter : public AudioWriter {
public:
    explicit OggFlacWriter(flac::EncoderOptions options = {});
    ~OggFlacWriter() override;
    OggFlacWriter(const OggFlacWriter&) = delete;
    OggFlacWriter& operator=(const OggFlacWriter&) = delete;

    std::string extension() const override { return "oga"; }
    std::string encoderDescription() const override { return "Ogg FLAC (built-in encoder), lossless"; }

    // As FlacWriter: CUESHEET block plus CUESHEET tag.
    bool canEmbedCueSheet() const override { return true; }
    void setEmbeddedCueSheet(const EmbeddedCueSheet& cue) override { cue_ = cue; }
    void open(const std::filesystem::path& path, const TrackMetadata& metadata) override;
    void write(const uint8_t* pcm, size_t bytes) override;
    void close() override;

    uint64_t totalSamples() const { return stream_.totalSamples(); }
    uint32_t serialNumber() const { return serial_; }

private:
    void onFrame(const std::vector<uint8_t>& frame, unsigned samples);
    // Writes the header packets to `ogg`; with `endOfStream` the last one ends the stream.
    void writeHeaders(OggStreamWriter& ogg, bool endOfStream) const;
    void writeBytes(const uint8_t* data, size_t size);

    flac::StreamEncoder stream_;
    std::ofstream out_;
    std::unique_ptr<OggStreamWriter> ogg_;
    uint32_t serial_ = 0;
    std::vector<std::vector<uint8_t>> headers_;  // header packets, the first one with a placeholder STREAMINFO
    uint64_t headerBytes_ = 0;                    // size of the header pages
    size_t leadOutOffsetPos_ = 0;                 // in headers_.back() (0: no CUESHEET)
    std::vector<uint8_t> frame_;                  // last frame, written once the next one (or the end) is known
    bool haveFrame_ = false;
    uint64_t framesEnd_ = 0;                      // samples up to the end of frame_
    std::optional<EmbeddedCueSheet> cue_;
};

}  // namespace cdr
