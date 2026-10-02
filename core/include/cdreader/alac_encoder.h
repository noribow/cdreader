#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cdreader/flac_encoder.h"  // flac::BitWriter (MSB-first, shared)

// Apple Lossless (ALAC) encoder for CD-DA audio (44.1 kHz, 16-bit, stereo),
// written from scratch so the core has no external dependencies (it is also
// built for Android), like the FLAC encoder. The bitstream follows Apple's
// open-sourced ALAC reference (https://github.com/macosforge/alac, Apache
// License 2.0): every frame is a channel pair element with
//
//   - stereo matrixing: u = (mixRes * L + (2^mixBits - mixRes) * R) >> mixBits,
//     v = L - R (mixRes 0 keeps L / R as they are),
//   - an adaptive (sign-sign LMS) predictor per channel, whose starting
//     coefficients are stored in the frame and which the decoder adapts in
//     exactly the same way while decoding,
//   - adaptive Golomb coding of the prediction residual, with run-length
//     coding of zeros,
//
// or is stored uncompressed ("escape" frame) when that is not smaller.
// AlacWriter (alac_writer.h) puts the frames into an MP4 / M4A file.
namespace cdr::alac {

constexpr unsigned kFrameLength = 4096;  // samples per channel in every frame but the last
constexpr uint32_t kSampleRate = 44100;
constexpr unsigned kBitDepth = 16;
constexpr unsigned kChannels = 2;
constexpr unsigned kMaxOrder = 30;  // predictor order is 5 bits; 31 selects a fixed first-order mode

// Tuning parameters of the adaptive Golomb coder, as recommended by Apple
// (and stored in the magic cookie).
constexpr unsigned kPb = 40;      // history multiplier
constexpr unsigned kMb = 10;      // initial history
constexpr unsigned kKb = 14;      // maximum Golomb parameter
constexpr unsigned kMaxRun = 255;
constexpr unsigned kDenShift = 9;  // predictor coefficient scale (2^9 = 1.0)
constexpr unsigned kMixBits = 2;   // stereo matrix scale

// ALACSpecificConfig: the 24-byte "magic cookie" a decoder needs, stored in
// the MP4 sample description (big-endian).
struct SpecificConfig {
    uint32_t frameLength = kFrameLength;
    uint8_t compatibleVersion = 0;
    uint8_t bitDepth = kBitDepth;
    uint8_t pb = kPb;
    uint8_t mb = kMb;
    uint8_t kb = kKb;
    uint8_t numChannels = kChannels;
    uint16_t maxRun = kMaxRun;
    uint32_t maxFrameBytes = 0;  // 0 = unknown
    uint32_t avgBitRate = 0;     // bit/s, 0 = unknown
    uint32_t sampleRate = kSampleRate;
};

std::array<uint8_t, 24> magicCookie(const SpecificConfig& config);

// Element tags of the frame syntax (3 bits each).
enum ElementTag : unsigned { kSingleChannel = 0, kChannelPair = 1, kEnd = 7 };

// Adaptive Golomb coding of `n` residuals of `bitSize`-bit values (17 for a
// 16-bit channel pair). Writes to `out` when it is not null and returns the
// number of bits. Sets `*ambiguous` (when not null) if the data hits a corner
// case in which decoders are known to disagree (a 0xFFFF value coded right
// after a run of zeros: Apple's decoder and FFmpeg clamp the history
// differently); the encoder then stores the frame uncompressed instead.
uint64_t adaptiveGolomb(const int32_t* residual, unsigned n, unsigned bitSize, flac::BitWriter* out,
                        bool* ambiguous = nullptr);

// Runs the adaptive predictor of `order` (1..kMaxOrder) over `n` samples of
// `in` (values of `chanBits` bits), writing the residual and adapting
// `coefs` in place, exactly as the decoder will. Returns false if the
// prediction hits a corner case in which decoders are known to disagree
// (32-bit overflow of the rounded prediction sum).
bool predict(const int32_t* in, unsigned n, int16_t* coefs, unsigned order, unsigned chanBits, int32_t* residual);

struct EncoderOptions {
    unsigned maxOrder = 16;           // predictor orders 4, 8 and 16 up to this are tried (and 0)
    bool stereoDecorrelation = true;  // try mixRes 1..4 besides L / R
    bool compress = true;             // false: uncompressed (escape) frames only
};

// What the last encode() chose, for tests and statistics.
struct FrameInfo {
    bool escaped = false;
    unsigned mixRes = 0;
    unsigned orderU = 0, orderV = 0;
};

// Encodes CD-DA audio (44.1 kHz, 16-bit, stereo) into ALAC frames. Per
// frame it picks the stereo matrix (mixRes 0..4), and per channel the
// predictor order (0, 4, 8, 16) and starting coefficients (least-squares fit
// of the frame, or the order-4 coefficients adapted in the previous frame as
// in Apple's encoder) giving the fewest bits; frames must therefore be
// encoded in order.
class FrameEncoder {
public:
    explicit FrameEncoder(EncoderOptions options = {});

    // Encodes one frame of `samples` (1..kFrameLength) samples per channel.
    // Returns the complete frame (channel pair element, end tag, byte aligned).
    std::vector<uint8_t> encode(const int32_t* left, const int32_t* right, unsigned samples);

    const FrameInfo& lastFrame() const { return info_; }

private:
    struct Choice {
        unsigned order = 0;
        uint64_t bits = 0;
        std::vector<int16_t> coefs;  // coefficients written to the frame
        std::vector<int32_t> residual;
    };
    bool chooseOrder(unsigned channel, const int32_t* x, unsigned n, Choice& best);
    std::vector<uint8_t> escape(const int32_t* left, const int32_t* right, unsigned samples);

    EncoderOptions options_;
    std::vector<unsigned> orders_;
    std::vector<int16_t> carried_[2];  // adapted order-4 coefficients per channel (u, v)
    std::vector<int32_t> u_, v_, scratch_;
    FrameInfo info_;
};

}  // namespace cdr::alac
