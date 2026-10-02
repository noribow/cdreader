#include "cdreader/flac_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cdr::flac {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kEscape = 255;           // marks an escaped (unencoded) Rice partition
constexpr unsigned kMaxPartitionOrder = 8;
constexpr int32_t kMaxResidual = 1 << 30;   // keeps zigzag values and escapes within 31 bits

const std::array<uint8_t, 256>& crc8Table() {
    static const std::array<uint8_t, 256> table = [] {
        std::array<uint8_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1);
            t[i] = uint8_t(c);
        }
        return t;
    }();
    return table;
}

const std::array<uint16_t, 256>& crc16Table() {
    static const std::array<uint16_t, 256> table = [] {
        std::array<uint16_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i << 8;
            for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? ((c << 1) ^ 0x8005) : (c << 1);
            t[i] = uint16_t(c);
        }
        return t;
    }();
    return table;
}

unsigned bitLength(uint64_t v) {
    unsigned n = 0;
    for (; v; v >>= 1) ++n;
    return n;
}

// floor(v / 2^shift), independent of how the compiler shifts negative values.
int64_t floorShift(int64_t v, unsigned shift) { return v >= 0 ? v >> shift : -((-v - 1) >> shift) - 1; }

int32_t floorHalf(int32_t v) { return int32_t(floorShift(v, 1)); }

// --- Rice coding ------------------------------------------------------------
//
// The residual of a subframe is split into 2^order partitions of equal size
// (the first one is shorter by the predictor order); each partition has its
// own Rice parameter or is stored "escaped" with a fixed number of bits.

struct PartitionStats {
    uint64_t sum = 0;  // sum of the zigzag values
    uint32_t max = 0;  // largest zigzag value
    unsigned count = 0;
};

unsigned partitionLimit(unsigned samples, unsigned predictorOrder, unsigned limit) {
    unsigned po = 0;
    while (po < limit && samples % (2u << po) == 0 && (samples >> (po + 1)) > predictorOrder) ++po;
    return po;
}

// Estimated bits of a partition's data (without the parameter field).
uint64_t estimatePartition(const PartitionStats& p, unsigned* parameter) {
    *parameter = 0;
    if (p.count == 0) return 0;
    const unsigned k0 = bitLength(p.sum / p.count);
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (unsigned k = k0 > 2 ? k0 - 2 : 0; k <= k0 + 1 && k <= 30; ++k) {
        const uint64_t bits = uint64_t(p.count) * (k + 1) + (p.sum >> k);
        if (bits < best) {
            best = bits;
            *parameter = k;
        }
    }
    const uint64_t escaped = 5 + uint64_t(p.count) * bitLength(p.max);
    if (escaped < best) {
        best = escaped;
        *parameter = kEscape;
    }
    return best;
}

PartitionStats statsOf(const uint32_t* u, unsigned count) {
    PartitionStats p;
    p.count = count;
    for (unsigned i = 0; i < count; ++i) {
        p.sum += u[i];
        p.max = std::max(p.max, u[i]);
    }
    return p;
}

// Estimated size of the residual section in bits; picks the partition order.
uint64_t estimateResidual(const uint32_t* u, unsigned samples, unsigned predictorOrder, unsigned maxOrder,
                          unsigned* partitionOrder) {
    const unsigned top = partitionLimit(samples, predictorOrder, maxOrder);
    PartitionStats parts[1u << kMaxPartitionOrder];
    const uint32_t* p = u;
    for (unsigned i = 0; i < (1u << top); ++i) {
        const unsigned count = (samples >> top) - (i == 0 ? predictorOrder : 0);
        parts[i] = statsOf(p, count);
        p += count;
    }
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (unsigned po = top;; --po) {
        uint64_t bits = 0;
        bool wide = false;  // needs 5-bit Rice parameters
        for (unsigned i = 0; i < (1u << po); ++i) {
            unsigned k = 0;
            bits += 4 + estimatePartition(parts[i], &k);
            if (k != kEscape && k > 14) wide = true;
        }
        if (wide) bits += 1u << po;
        if (bits < best) {
            best = bits;
            *partitionOrder = po;
        }
        if (po == 0) break;
        for (unsigned i = 0; i < (1u << (po - 1)); ++i) {
            const PartitionStats& a = parts[2 * i];
            const PartitionStats& b = parts[2 * i + 1];
            parts[i] = {a.sum + b.sum, std::max(a.max, b.max), a.count + b.count};
        }
    }
    return 6 + best;
}

// Writes the residual section with exactly chosen Rice parameters.
void writeResidual(BitWriter& out, const uint32_t* u, unsigned samples, unsigned predictorOrder,
                   unsigned partitionOrder) {
    const unsigned parts = 1u << partitionOrder;
    unsigned params[1u << kMaxPartitionOrder];
    unsigned rawBits[1u << kMaxPartitionOrder];
    bool wide = false;
    const uint32_t* p = u;
    for (unsigned i = 0; i < parts; ++i) {
        const unsigned count = (samples >> partitionOrder) - (i == 0 ? predictorOrder : 0);
        const PartitionStats s = statsOf(p, count);
        params[i] = 0;
        rawBits[i] = bitLength(s.max);
        if (count) {
            const unsigned k0 = bitLength(s.sum / count);
            uint64_t best = 5 + uint64_t(count) * rawBits[i];
            params[i] = kEscape;
            for (unsigned k = k0 > 2 ? k0 - 2 : 0; k <= k0 + 1 && k <= 30; ++k) {
                uint64_t bits = uint64_t(count) * (k + 1);
                for (unsigned j = 0; j < count; ++j) bits += p[j] >> k;
                if (bits < best) {
                    best = bits;
                    params[i] = k;
                }
            }
            if (params[i] != kEscape && params[i] > 14) wide = true;
        }
        p += count;
    }

    const unsigned paramBits = wide ? 5 : 4;
    out.writeBits(wide ? 1 : 0, 2);  // residual coding method
    out.writeBits(partitionOrder, 4);
    p = u;
    for (unsigned i = 0; i < parts; ++i) {
        const unsigned count = (samples >> partitionOrder) - (i == 0 ? predictorOrder : 0);
        if (params[i] == kEscape) {
            out.writeBits((1u << paramBits) - 1, paramBits);
            out.writeBits(rawBits[i], 5);
            for (unsigned j = 0; j < count; ++j) {
                const int32_t v = int32_t(p[j] >> 1) ^ -int32_t(p[j] & 1);  // undo zigzag
                out.writeSigned(v, rawBits[i]);
            }
        } else {
            const unsigned k = params[i];
            const uint32_t mask = (1u << k) - 1;
            out.writeBits(k, paramBits);
            for (unsigned j = 0; j < count; ++j) {
                out.writeUnary(p[j] >> k);
                out.writeBits(p[j] & mask, k);
            }
        }
        p += count;
    }
}

// --- Prediction ---------------------------------------------------------------

void fixedResidual(const int32_t* x, unsigned n, unsigned order, uint32_t* u) {
    for (unsigned i = order; i < n; ++i) {
        int32_t r;
        switch (order) {
        case 0: r = x[i]; break;
        case 1: r = x[i] - x[i - 1]; break;
        case 2: r = x[i] - 2 * x[i - 1] + x[i - 2]; break;
        case 3: r = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
        default: r = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
        }
        u[i - order] = zigzag(r);
    }
}

// Levinson-Durbin recursion: lp[o - 1] holds the predictor of order o and
// error[o - 1] its prediction error. Returns the highest usable order.
unsigned levinson(const double* autoc, unsigned maxOrder, double lp[kMaxLpcOrder][kMaxLpcOrder],
                  double* error) {
    double a[kMaxLpcOrder] = {};
    double err = autoc[0];
    for (unsigned i = 0; i < maxOrder; ++i) {
        double r = -autoc[i + 1];
        for (unsigned j = 0; j < i; ++j) r -= a[j] * autoc[i - j];
        r /= err;
        a[i] = r;
        unsigned j = 0;
        for (; j < i / 2; ++j) {
            const double tmp = a[j];
            a[j] += r * a[i - 1 - j];
            a[i - 1 - j] += r * tmp;
        }
        if (i & 1) a[j] += a[j] * r;
        err *= 1.0 - r * r;
        for (j = 0; j <= i; ++j) lp[i][j] = -a[j];
        error[i] = err;
        if (!(err > 0.0)) return i + 1;
    }
    return maxOrder;
}

// Quantizes predictor coefficients to `precision` bits (sign included).
// Returns false when they cannot be represented with a shift of 0..15.
bool quantize(const double* lp, unsigned order, unsigned precision, int32_t* q, unsigned* shiftOut) {
    double cmax = 0;
    for (unsigned i = 0; i < order; ++i) {
        if (!std::isfinite(lp[i])) return false;
        cmax = std::max(cmax, std::fabs(lp[i]));
    }
    if (!(cmax > 0)) return false;
    const int32_t qmax = (1 << (precision - 1)) - 1;
    const int32_t qmin = -qmax - 1;
    int log2cmax = 0;
    std::frexp(cmax, &log2cmax);
    int shift = int(precision) - log2cmax - 1;  // the largest coefficient uses all bits
    if (shift > 15) shift = 15;
    if (shift < 0) return false;
    double error = 0;
    for (unsigned i = 0; i < order; ++i) {
        error += lp[i] * double(1 << shift);
        long v = std::lround(error);
        v = std::clamp<long>(v, qmin, qmax);
        error -= double(v);
        q[i] = int32_t(v);
    }
    *shiftOut = unsigned(shift);
    return true;
}

bool lpcResidual(const int32_t* x, unsigned n, const int32_t* q, unsigned order, unsigned shift, uint32_t* u) {
    for (unsigned i = order; i < n; ++i) {
        int64_t sum = 0;
        for (unsigned j = 0; j < order; ++j) sum += int64_t(q[j]) * x[i - 1 - j];
        const int64_t r = int64_t(x[i]) - floorShift(sum, shift);
        if (r < -kMaxResidual || r >= kMaxResidual) return false;
        u[i - order] = zigzag(int32_t(r));
    }
    return true;
}

unsigned lpcPrecision(unsigned n) {
    if (n <= 192) return 7;
    if (n <= 384) return 8;
    if (n <= 576) return 9;
    if (n <= 1152) return 10;
    if (n <= 2304) return 11;
    if (n <= 4608) return 12;
    return 13;
}

unsigned blockSizeCode(unsigned n) {
    for (unsigned code = 8; code <= 15; ++code)
        if (n == 256u << (code - 8)) return code;
    return n <= 256 ? 6 : 7;
}

}  // namespace

uint8_t crc8(const uint8_t* data, size_t length) {
    const auto& table = crc8Table();
    uint8_t c = 0;
    for (size_t i = 0; i < length; ++i) c = table[c ^ data[i]];
    return c;
}

uint16_t crc16(const uint8_t* data, size_t length) {
    const auto& table = crc16Table();
    unsigned c = 0;
    for (size_t i = 0; i < length; ++i) c = ((c << 8) ^ table[((c >> 8) ^ data[i]) & 0xFF]) & 0xFFFF;
    return uint16_t(c);
}

// --- BitWriter ----------------------------------------------------------------

void BitWriter::writeBits(uint32_t value, unsigned bits) {
    if (bits == 0) return;
    if (bits < 32) value &= (1u << bits) - 1;
    acc_ = (acc_ << bits) | value;
    pending_ += bits;
    while (pending_ >= 8) {
        pending_ -= 8;
        bytes_.push_back(uint8_t(acc_ >> pending_));
    }
    acc_ &= (uint64_t(1) << pending_) - 1;
}

void BitWriter::writeSigned(int32_t value, unsigned bits) { writeBits(uint32_t(value), bits); }

void BitWriter::writeUnary(uint32_t zeros) {
    for (; zeros >= 32; zeros -= 32) writeBits(0, 32);
    writeBits(1, zeros + 1);
}

void BitWriter::writeRice(int32_t value, unsigned parameter) {
    const uint32_t u = zigzag(value);
    writeUnary(u >> parameter);
    writeBits(u, parameter);
}

void BitWriter::writeUtf8(uint32_t value) {
    if (value < 0x80) {
        writeBits(value, 8);
        return;
    }
    if (value > 0x7FFFFFFF) throw std::length_error("FLAC frame number out of range");
    unsigned bytes = 2;
    while (bytes < 6 && value >= (1u << (5 * bytes + 1))) ++bytes;
    writeBits(((0xFFu << (8 - bytes)) & 0xFF) | (value >> (6 * (bytes - 1))), 8);
    for (unsigned i = bytes - 1; i-- > 0;) writeBits(0x80 | ((value >> (6 * i)) & 0x3F), 8);
}

void BitWriter::append(const BitWriter& other) {
    for (uint8_t b : other.bytes_) writeBits(b, 8);
    writeBits(uint32_t(other.acc_), other.pending_);
}

void BitWriter::alignToByte() {
    if (pending_) writeBits(0, 8 - pending_);
}

// --- FrameEncoder -------------------------------------------------------------

FrameEncoder::FrameEncoder(EncoderOptions options) : options_(options) {
    options_.maxLpcOrder = std::min(options_.maxLpcOrder, kMaxLpcOrder);
    options_.maxPartitionOrder = std::min(options_.maxPartitionOrder, kMaxPartitionOrder);
}

void FrameEncoder::encodeSubframe(const int32_t* data, unsigned n, unsigned bps, BitWriter& out) {
    if (std::all_of(data, data + n, [&](int32_t v) { return v == data[0]; })) {
        out.writeBits(0, 8);  // CONSTANT, no wasted bits
        out.writeSigned(data[0], bps);
        return;
    }

    // "Wasted bits": low bits that are zero in every sample are not coded.
    uint32_t bitsUsed = 0;
    for (unsigned i = 0; i < n; ++i) bitsUsed |= uint32_t(data[i]);
    unsigned wasted = 0;
    for (; !(bitsUsed & 1); bitsUsed >>= 1) ++wasted;
    const int32_t* x = data;
    if (wasted) {
        shifted_.resize(n);
        for (unsigned i = 0; i < n; ++i) shifted_[i] = data[i] / (int32_t(1) << wasted);  // exact
        x = shifted_.data();
        bps -= wasted;
    }

    enum class Kind { Verbatim, Fixed, Lpc } kind = Kind::Verbatim;
    uint64_t bestBits = uint64_t(n) * bps;
    unsigned bestOrder = 0, bestPartitionOrder = 0;
    int32_t coefs[kMaxLpcOrder] = {};
    unsigned precision = 0, shift = 0;

    residual_.resize(n);
    bestResidual_.resize(n);
    const unsigned maxFixed = std::min(4u, n - 1);
    for (unsigned order = 0; order <= maxFixed; ++order) {
        fixedResidual(x, n, order, residual_.data());
        unsigned po = 0;
        const uint64_t bits =
            uint64_t(order) * bps + estimateResidual(residual_.data(), n, order, options_.maxPartitionOrder, &po);
        if (bits < bestBits) {
            bestBits = bits;
            kind = Kind::Fixed;
            bestOrder = order;
            bestPartitionOrder = po;
            std::swap(residual_, bestResidual_);
        }
    }

    const unsigned maxLpc = std::min(options_.maxLpcOrder, n / 8);
    if (maxLpc > 0) {
        // Tukey(0.5) window, autocorrelation and Levinson-Durbin.
        if (window_.size() != n) {
            window_.assign(n, 1.0);
            const unsigned taper = n / 4;
            for (unsigned i = 0; i < taper; ++i) {
                const double w = 0.5 - 0.5 * std::cos(kPi * i / taper);
                window_[i] = w;
                window_[n - 1 - i] = w;
            }
        }
        windowed_.resize(n);
        for (unsigned i = 0; i < n; ++i) windowed_[i] = x[i] * window_[i];
        double autoc[kMaxLpcOrder + 1];
        for (unsigned lag = 0; lag <= maxLpc; ++lag) {
            double s = 0;
            for (unsigned i = lag; i < n; ++i) s += windowed_[i] * windowed_[i - lag];
            autoc[lag] = s;
        }
        if (autoc[0] > 0) {
            double lp[kMaxLpcOrder][kMaxLpcOrder];
            double error[kMaxLpcOrder];
            const unsigned orders = levinson(autoc, maxLpc, lp, error);
            const unsigned prec = lpcPrecision(n);

            // Estimate the best order from the prediction error, then also try the highest one.
            unsigned guess = 1;
            double guessBits = std::numeric_limits<double>::max();
            for (unsigned o = 1; o <= orders; ++o) {
                const double e = error[o - 1] * 0.5 / n;
                const double perSample = e > 0 ? std::max(0.0, 0.5 * std::log2(e)) : 0.0;
                const double bits = perSample * (n - o) + double(o) * (bps + prec);
                if (bits < guessBits) {
                    guessBits = bits;
                    guess = o;
                }
            }
            for (unsigned order : {guess, orders}) {
                if (order == bestOrder && kind == Kind::Lpc) continue;
                int32_t q[kMaxLpcOrder] = {};
                unsigned s = 0;
                if (!quantize(lp[order - 1], order, prec, q, &s)) continue;
                if (!lpcResidual(x, n, q, order, s, residual_.data())) continue;
                unsigned po = 0;
                const uint64_t bits = uint64_t(order) * (bps + prec) + 9 +
                                      estimateResidual(residual_.data(), n, order, options_.maxPartitionOrder, &po);
                if (bits < bestBits) {
                    bestBits = bits;
                    kind = Kind::Lpc;
                    bestOrder = order;
                    bestPartitionOrder = po;
                    precision = prec;
                    shift = s;
                    std::copy(q, q + order, coefs);
                    std::swap(residual_, bestResidual_);
                }
            }
        }
    }

    // Subframe header: zero bit, type, wasted bits flag (+ unary count).
    const unsigned type = kind == Kind::Verbatim ? 1 : kind == Kind::Fixed ? 8 + bestOrder : 32 + bestOrder - 1;
    out.writeBits(type, 7);
    if (wasted) {
        out.writeBits(1, 1);
        out.writeUnary(wasted - 1);
    } else {
        out.writeBits(0, 1);
    }

    if (kind == Kind::Verbatim) {
        for (unsigned i = 0; i < n; ++i) out.writeSigned(x[i], bps);
        return;
    }
    for (unsigned i = 0; i < bestOrder; ++i) out.writeSigned(x[i], bps);  // warm-up samples
    if (kind == Kind::Lpc) {
        out.writeBits(precision - 1, 4);
        out.writeBits(shift, 5);
        for (unsigned i = 0; i < bestOrder; ++i) out.writeSigned(coefs[i], precision);
    }
    writeResidual(out, bestResidual_.data(), n, bestOrder, bestPartitionOrder);
}

std::vector<uint8_t> FrameEncoder::encode(const int32_t* left, const int32_t* right, unsigned n,
                                          uint32_t frameNumber) {
    if (n == 0 || n > 65535) throw std::invalid_argument("invalid FLAC block size");

    BitWriter sub[4];  // left, right, mid, side
    encodeSubframe(left, n, 16, sub[0]);
    encodeSubframe(right, n, 16, sub[1]);
    unsigned assignment = 1;  // independent stereo
    const BitWriter* first = &sub[0];
    const BitWriter* second = &sub[1];
    if (options_.stereoDecorrelation) {
        mid_.resize(n);
        side_.resize(n);
        for (unsigned i = 0; i < n; ++i) {
            mid_[i] = floorHalf(left[i] + right[i]);
            side_[i] = left[i] - right[i];
        }
        encodeSubframe(mid_.data(), n, 16, sub[2]);
        encodeSubframe(side_.data(), n, 17, sub[3]);  // side needs one extra bit
        const uint64_t l = sub[0].bitCount(), r = sub[1].bitCount(), m = sub[2].bitCount(), s = sub[3].bitCount();
        uint64_t best = l + r;
        if (l + s < best) {
            best = l + s;
            assignment = 8;  // left/side
            first = &sub[0];
            second = &sub[3];
        }
        if (s + r < best) {
            best = s + r;
            assignment = 9;  // side/right
            first = &sub[3];
            second = &sub[1];
        }
        if (m + s < best) {
            assignment = 10;  // mid/side
            first = &sub[2];
            second = &sub[3];
        }
    }

    BitWriter frame;
    const unsigned bsCode = blockSizeCode(n);
    frame.writeBits(0xFFF8, 16);  // sync code, reserved bit, fixed block size stream
    frame.writeBits(bsCode, 4);
    frame.writeBits(9, 4);  // 44.1 kHz
    frame.writeBits(assignment, 4);
    frame.writeBits(4, 3);  // 16 bits per sample
    frame.writeBits(0, 1);
    frame.writeUtf8(frameNumber);
    if (bsCode == 6) frame.writeBits(n - 1, 8);
    if (bsCode == 7) frame.writeBits(n - 1, 16);
    frame.writeBits(crc8(frame.bytes().data(), frame.bytes().size()), 8);

    frame.append(*first);
    frame.append(*second);
    frame.alignToByte();
    frame.writeBits(crc16(frame.bytes().data(), frame.bytes().size()), 16);
    return frame.bytes();
}

// --- Metadata -----------------------------------------------------------------

namespace {

// CUESHEET layout (FLAC format, METADATA_BLOCK_CUESHEET).
constexpr size_t kCueHeaderBytes = 128 + 8 + 1 + 258 + 1;  // MCN, lead-in, flags, reserved, track count
constexpr size_t kCueTrackBytes = 8 + 1 + 12 + 1 + 13 + 1;  // offset, number, ISRC, flags, reserved, index count
constexpr size_t kCueIndexBytes = 8 + 1 + 3;                // offset, number, reserved
constexpr uint64_t kCdLeadInSamples = 2 * 44100;            // the 2-second pregap before LBA 0
constexpr uint8_t kLeadOutTrack = 170;

void putBigEndian(std::vector<uint8_t>& v, uint64_t x, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

size_t cueSheetLeadOutOffsetPosition(size_t tracks) {
    return kCueHeaderBytes + tracks * (kCueTrackBytes + kCueIndexBytes);
}

std::vector<uint8_t> cueSheet(const EmbeddedCueSheet& cue, uint64_t leadOutSamples) {
    if (cue.tracks.size() > 99) throw std::invalid_argument("a CD has at most 99 tracks");
    const bool isCd = leadOutSamples % kSamplesPerSector == 0;
    for (const CueTrack& t : cue.tracks) {
        if (t.number < 1 || t.number > 99) throw std::invalid_argument("CD track numbers are 1..99");
        if (uint64_t(t.startSectors) * kSamplesPerSector >= leadOutSamples && leadOutSamples > 0)
            throw std::invalid_argument("CUE track starts after the end of the image");
    }

    std::vector<uint8_t> v(128, 0);  // media catalog number: unknown
    putBigEndian(v, isCd ? kCdLeadInSamples : 0, 8);
    v.push_back(isCd ? 0x80 : 0x00);
    v.resize(v.size() + 258, 0);
    v.push_back(uint8_t(cue.tracks.size() + 1));
    for (const CueTrack& t : cue.tracks) {
        putBigEndian(v, uint64_t(t.startSectors) * kSamplesPerSector, 8);
        v.push_back(uint8_t(t.number));
        v.resize(v.size() + 12, 0);                          // ISRC: unknown
        v.push_back(uint8_t(t.preEmphasis ? 0x40 : 0x00));   // audio track, pre-emphasis flag
        v.resize(v.size() + 13, 0);
        v.push_back(1);                                      // one index point: INDEX 01 at the track start
        putBigEndian(v, 0, 8);
        v.push_back(1);
        v.resize(v.size() + 3, 0);
    }
    putBigEndian(v, leadOutSamples, 8);
    v.push_back(kLeadOutTrack);
    v.resize(v.size() + 12 + 1 + 13, 0);
    v.push_back(0);  // the lead-out has no index points
    return v;
}

}  // namespace cdr::flac
