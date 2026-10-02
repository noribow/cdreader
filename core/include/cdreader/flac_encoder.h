#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/md5.h"
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
// marked as CD-DA when every offset is a multiple of 588.
std::vector<uint8_t> cueSheet(const EmbeddedCueSheet& cue, uint64_t leadOutSamples);

// Position of the lead-out track's 64-bit offset inside a cueSheet() body.
size_t cueSheetLeadOutOffsetPosition(size_t tracks);

// Value of the CUESHEET tag: the CUE sheet text without the .cue file's
// UTF-8 BOM (the tag holds plain UTF-8).
std::string cueSheetTagText(const std::string& cueText);

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kStreamInfoBytes = 34;

// Metadata block types (RFC 9639 section 8.1).
enum BlockType : uint8_t { kStreamInfo = 0, kPadding = 1, kSeekTable = 3, kVorbisComment = 4, kCueSheet = 5 };

// Appends a metadata block header: last-block flag, type, 24-bit body length.
void putBlockHeader(std::vector<uint8_t>& out, BlockType type, bool last, uint32_t length);

// Appends `x` as a `bytes`-byte big-endian number.
void putBigEndian(std::vector<uint8_t>& out, uint64_t x, int bytes);

// Splits a stream of CD-DA PCM (16-bit stereo little-endian, any chunking)
// into blocks of kBlockSize samples, encodes them with FrameEncoder and keeps
// what STREAMINFO needs: sample count, frame size bounds and the MD5 of the
// PCM. Shared by the native FLAC and the Ogg FLAC writer.
class StreamEncoder {
public:
    // Receives each encoded frame (numbered from 0) with its sample count.
    using FrameSink = std::function<void(const std::vector<uint8_t>& frame, unsigned samples)>;

    explicit StreamEncoder(EncoderOptions options = {}) : encoder_(options) {}

    void start(FrameSink sink);  // begins a new stream
    void write(const uint8_t* pcm, size_t bytes);
    // Whether the input so far ends on a sample boundary (finish() requires it).
    bool wholeSamples() const { return pending_.size() % kBytesPerSample == 0; }
    // Encodes the remaining samples as a last, shorter frame and computes the MD5.
    void finish();

    uint64_t totalSamples() const { return totalSamples_; }
    uint32_t frames() const { return frames_; }
    // Body of the STREAMINFO block (kStreamInfoBytes bytes), valid after
    // finish(). Throws std::runtime_error for a stream too long for FLAC.
    std::vector<uint8_t> streamInfo() const;

    static constexpr unsigned kBytesPerSample = 4;  // one 16-bit stereo sample

private:
    void encodeBlock(const uint8_t* pcm, unsigned samples);

    FrameEncoder encoder_;
    FrameSink sink_;
    std::vector<uint8_t> pending_;  // PCM bytes not yet encoded (less than one block)
    std::vector<int32_t> left_, right_;
    Md5 md5_;
    std::array<uint8_t, 16> digest_{};
    uint64_t totalSamples_ = 0;
    uint32_t frames_ = 0;
    uint32_t minFrameBytes_ = 0, maxFrameBytes_ = 0;
};

}  // namespace cdr::flac
