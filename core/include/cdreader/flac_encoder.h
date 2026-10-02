#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/metadata.h"
#include "cdreader/tags.h"

// Building blocks of the FLAC encoder (https://www.rfc-editor.org/rfc/rfc9639),
// written from scratch so the core has no external dependencies (it is also
// built for Android). FlacWriter (flac_writer.h) puts them together.
namespace cdr::flac {

constexpr unsigned kBlockSize = 4096;  // samples per channel in every frame but the last
constexpr unsigned kMaxLpcOrder = 32;

uint8_t crc8(const uint8_t* data, size_t length);    // frame header CRC (poly 0x07)
uint16_t crc16(const uint8_t* data, size_t length);  // frame footer CRC (poly 0x8005)

// MSB-first bit writer.
class BitWriter {
public:
    void writeBits(uint32_t value, unsigned bits);  // bits <= 32
    void writeSigned(int32_t value, unsigned bits); // two's complement, bits <= 32
    void writeUnary(uint32_t zeros);                // `zeros` 0-bits followed by a 1-bit
    void writeRice(int32_t value, unsigned parameter);
    void writeUtf8(uint32_t value);                 // FLAC frame number coding (up to 31 bits)
    void append(const BitWriter& other);
    void alignToByte();                             // pads with zero bits

    uint64_t bitCount() const { return uint64_t(bytes_.size()) * 8 + pending_; }
    // Complete bytes written so far (everything once the writer is byte aligned).
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    uint64_t acc_ = 0;      // the `pending_` low bits are not yet in bytes_
    unsigned pending_ = 0;  // always < 8 between calls
};

// Maps a signed residual to the unsigned value coded by Rice codes (0,-1,1,-2.. -> 0,1,2,3..).
inline uint32_t zigzag(int32_t v) {
    const uint32_t u = uint32_t(v);
    return (u << 1) ^ (0u - (u >> 31));
}

struct EncoderOptions {
    unsigned maxLpcOrder = 8;        // 0 = FIXED predictors only
    unsigned maxPartitionOrder = 6;  // Rice partition search depth (0..8)
    bool stereoDecorrelation = true; // try left/side, right/side and mid/side
};

// Encodes CD-DA audio (44.1 kHz, 16-bit, stereo) into FLAC frames.
class FrameEncoder {
public:
    explicit FrameEncoder(EncoderOptions options = {});

    // Encodes one frame of `samples` (1..65535) samples per channel. Returns the
    // complete frame (header, two subframes, CRC-16 footer).
    std::vector<uint8_t> encode(const int32_t* left, const int32_t* right, unsigned samples,
                                uint32_t frameNumber);

    // Appends one subframe for `samples` values of `bitsPerSample` bits.
    void encodeSubframe(const int32_t* data, unsigned samples, unsigned bitsPerSample, BitWriter& out);

private:
    EncoderOptions options_;
    std::vector<int32_t> mid_, side_, shifted_;
    std::vector<uint32_t> residual_, bestResidual_;
    std::vector<double> window_, windowed_;
};

// Body of a VORBIS_COMMENT metadata block (without the 4-byte block header),
// see cdr::vorbisComment() in tags.h.
inline std::vector<uint8_t> vorbisComment(const TrackMetadata& metadata, const std::string& vendor,
                                          const std::string& cueSheet = {}) {
    return cdr::vorbisComment(metadata, vendor, cueSheet);
}

// Body of a CUESHEET metadata block for a CD image: one track per CueTrack
// with INDEX 01 at its start, then the lead-out track (number 170) at
// `leadOutSamples`. Offsets are in samples (588 per CD frame); the block is
// marked as CD-DA when every offset is a multiple of 588. A track with
// INDEX 00 in the file (#25) starts at INDEX 00, with index points 0 (offset
// 0) and 1 (the pregap length), as metaflac --import-cuesheet-from builds
// them; INDEX 02+ follow.
std::vector<uint8_t> cueSheet(const EmbeddedCueSheet& cue, uint64_t leadOutSamples);

// Position of the lead-out track's 64-bit offset inside a cueSheet() body:
// for `tracks` tracks with one index point each, or for `cue`.
size_t cueSheetLeadOutOffsetPosition(size_t tracks);
size_t cueSheetLeadOutOffsetPosition(const EmbeddedCueSheet& cue);

}  // namespace cdr::flac
