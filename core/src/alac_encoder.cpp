#include "cdreader/alac_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cdr::alac {

namespace {

constexpr unsigned kMaxPrefix = 9;        // a Golomb code with more leading 1-bits is an escape
constexpr unsigned kMaxCodeBits = 25;     // longer codes are escaped as well
constexpr uint32_t kHistoryClamp = 0xFFFF;
constexpr unsigned kMaxZeroRun = 65535;
constexpr unsigned kPbFactor = 4;         // history multiplier = kPb * kPbFactor / 4
constexpr unsigned kMaxMixRes = 4;        // mixRes 0..2^kMixBits
constexpr unsigned kConvergePasses = 8;   // passes of the predictor over the start of a frame
constexpr unsigned kCarriedOrder = 4;     // order whose adapted coefficients carry over between frames

// floor(v / 2^shift), independent of how the compiler shifts negative values.
int64_t floorShift(int64_t v, unsigned shift) { return v >= 0 ? v >> shift : -((-v - 1) >> shift) - 1; }

// The low `bits` bits of v as a signed value (two's complement wrap-around).
int32_t signExtend(int64_t v, unsigned bits) {
    const uint64_t mask = (uint64_t(1) << bits) - 1;
    const uint64_t u = uint64_t(v) & mask;
    return int32_t(u >> (bits - 1) ? int64_t(u) - (int64_t(1) << bits) : int64_t(u));
}

// int16_t arithmetic of the reference decoder (coefficients wrap around).
int16_t wrap16(int32_t v) { return int16_t(signExtend(v, 16)); }

int32_t signOf(int64_t v) { return (v > 0) - (v < 0); }

unsigned floorLog2(uint32_t v) {  // v > 0
    unsigned n = 0;
    for (unsigned shift : {16u, 8u, 4u, 2u, 1u}) {
        if (v >> shift) {
            v >>= shift;
            n += shift;
        }
    }
    return n;
}

// u = (mixRes * L + (2^mixBits - mixRes) * R) >> mixBits, v = L - R; mixRes 0: u = L, v = R.
void mix(const int32_t* l, const int32_t* r, unsigned n, unsigned mixRes, int32_t* u, int32_t* v) {
    if (mixRes == 0) {
        std::copy(l, l + n, u);
        std::copy(r, r + n, v);
        return;
    }
    const int64_t other = (int64_t(1) << kMixBits) - mixRes;
    for (unsigned i = 0; i < n; ++i) {
        u[i] = int32_t(floorShift(int64_t(mixRes) * l[i] + other * r[i], kMixBits));
        v[i] = l[i] - r[i];
    }
}

// Least-squares starting coefficients for the predictor of `order`: the
// prediction of x[j] is top + sum(c[k] * (x[j-1-k] - top)) / 2^kDenShift with
// top = x[j-1-order], so c solves the normal equations of the differences
// to `top` over the frame (covariance method, Cholesky). Returns false when
// the system is singular (e.g. silence).
bool leastSquares(const int32_t* x, unsigned n, unsigned order, int16_t* coefs) {
    std::vector<double> a(size_t(order) * order, 0.0), b(order, 0.0), z(order);
    // Every other sample is plenty for a few coefficients and halves the work.
    for (unsigned j = order + 1; j < n; j += 2) {
        const double top = x[j - order - 1];
        for (unsigned k = 0; k < order; ++k) z[k] = x[j - 1 - k] - top;
        const double y = x[j] - top;
        for (unsigned k = 0; k < order; ++k) {
            b[k] += z[k] * y;
            for (unsigned m = 0; m <= k; ++m) a[size_t(k) * order + m] += z[k] * z[m];
        }
    }
    // Cholesky decomposition A = L L^T in place (lower triangle), slightly regularized.
    for (unsigned k = 0; k < order; ++k) a[size_t(k) * order + k] *= 1.0 + 1e-9;
    for (unsigned k = 0; k < order; ++k) {
        for (unsigned m = 0; m <= k; ++m) {
            double sum = a[size_t(k) * order + m];
            for (unsigned i = 0; i < m; ++i) sum -= a[size_t(k) * order + i] * a[size_t(m) * order + i];
            if (m == k) {
                if (!(sum > 1e-6)) return false;
                a[size_t(k) * order + k] = std::sqrt(sum);
            } else {
                a[size_t(k) * order + m] = sum / a[size_t(m) * order + m];
            }
        }
    }
    for (unsigned k = 0; k < order; ++k) {  // L y = b
        double sum = b[k];
        for (unsigned i = 0; i < k; ++i) sum -= a[size_t(k) * order + i] * b[i];
        b[k] = sum / a[size_t(k) * order + k];
    }
    for (unsigned k = order; k-- > 0;) {  // L^T c = y
        double sum = b[k];
        for (unsigned i = k + 1; i < order; ++i) sum -= a[size_t(i) * order + k] * b[i];
        b[k] = sum / a[size_t(k) * order + k];
    }
    for (unsigned k = 0; k < order; ++k) {
        const double c = b[k] * double(1 << kDenShift);
        if (!std::isfinite(c)) return false;
        coefs[k] = int16_t(std::clamp(std::lround(c), -32768L, 32767L));
    }
    return true;
}

}  // namespace

std::array<uint8_t, 24> magicCookie(const SpecificConfig& c) {
    std::array<uint8_t, 24> b{};
    auto put = [&](size_t pos, uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) b[pos + size_t(i)] = uint8_t(v >> (8 * (bytes - 1 - i)));
    };
    put(0, c.frameLength, 4);
    b[4] = c.compatibleVersion;
    b[5] = c.bitDepth;
    b[6] = c.pb;
    b[7] = c.mb;
    b[8] = c.kb;
    b[9] = c.numChannels;
    put(10, c.maxRun, 2);
    put(12, c.maxFrameBytes, 4);
    put(16, c.avgBitRate, 4);
    put(20, c.sampleRate, 4);
    return b;
}

// --- Adaptive Golomb coding -------------------------------------------------
//
// Each value is coded with a Golomb code of modulus m = 2^k - 1, where k
// follows a running mean ("history") of the coded values. When the history
// gets small, a run of zeros is coded as one count; the value after the run
// is then coded minus one (it cannot be zero).

uint64_t adaptiveGolomb(const int32_t* pc, unsigned n, unsigned bitSize, flac::BitWriter* out, bool* ambiguous) {
    uint64_t bits = 0;
    auto emit = [&](uint32_t value, unsigned count) {
        bits += count;
        if (out) out->writeBits(value, count);
    };
    // Golomb code of `value` with modulus m = 2^k - 1: `value / m` 1-bits, a
    // 0-bit, then the remainder in k bits (stored as remainder + 1, or in
    // k - 1 bits when it is zero). Large values: nine 1-bits and the value.
    auto code = [&](uint32_t m, unsigned k, uint32_t value, unsigned escapeBits) {
        const uint32_t div = value / m;
        if (div < kMaxPrefix) {
            const uint32_t mod = value - m * div;
            const unsigned zero = mod == 0;
            const unsigned count = div + k + 1 - zero;
            if (count <= kMaxCodeBits) {
                emit((((1u << div) - 1) << (count - div)) + mod + 1 - zero, count);
                return;
            }
        }
        emit((1u << kMaxPrefix) - 1, kMaxPrefix);
        emit(value, escapeBits);
    };
    auto flag = [&] {
        if (ambiguous) *ambiguous = true;
    };

    uint32_t history = kMb;
    uint32_t zmode = 0;
    unsigned c = 0;
    while (c < n) {
        const unsigned k = std::min(floorLog2((history >> 9) + 3), kKb);
        const int32_t del = pc[c++];
        const uint32_t magnitude = del < 0 ? uint32_t(-int64_t(del)) : uint32_t(del);
        const uint32_t value = (magnitude << 1) - (del < 0 ? 1 : 0) - zmode;
        if (zmode && value == kHistoryClamp) flag();
        code((1u << k) - 1, k, value, bitSize);

        history = kPb * (value + zmode) + history - ((kPb * history) >> 9);
        if (value > kHistoryClamp) history = kHistoryClamp;
        zmode = 0;
        if ((history << 2) < 512 && c < n) {
            zmode = 1;
            uint32_t zeros = 0;
            while (c < n && pc[c] == 0) {
                ++c;
                if (++zeros >= kMaxZeroRun) {
                    zmode = 0;
                    flag();
                    break;
                }
            }
            if (history == 0) flag();
            const unsigned kz = (history ? 31 - floorLog2(history) : 32) - 24 + ((history + 16) >> 6);
            code(((1u << kz) - 1) & ((1u << kKb) - 1), kz, zeros, 16);
            history = 0;
        }
    }
    return bits;
}

// --- Adaptive prediction -----------------------------------------------------
//
// Sample j is predicted from the `order` previous ones relative to the oldest
// sample used ("top"); after each sample the coefficients move by one step
// towards a smaller error (sign-sign LMS), starting with the oldest tap,
// until the error has been "used up".

bool predict(const int32_t* in, unsigned n, int16_t* coefs, unsigned order, unsigned chanBits, int32_t* res) {
    if (n == 0) return true;
    res[0] = in[0];
    if (order == 0) {
        std::copy(in + 1, in + n, res + 1);
        return true;
    }
    for (unsigned j = 1; j <= order && j < n; ++j) res[j] = signExtend(int64_t(in[j]) - in[j - 1], chanBits);

    bool ok = true;
    constexpr int64_t denHalf = int64_t(1) << (kDenShift - 1);
    for (unsigned j = order + 1; j < n; ++j) {
        const int32_t top = in[j - order - 1];
        const int32_t* past = in + j - 1;  // past[-k]: the sample k + 1 before j
        // The reference decoder sums in 32-bit integers, wrapping around, and
        // adds the rounding term before shifting: when that addition
        // overflows, decoders that widen it disagree.
        uint32_t sum = 0;
        for (unsigned k = 0; k < order; ++k) sum += uint32_t(int32_t(coefs[k])) * uint32_t(past[-int(k)] - top);
        const int64_t sum32 = signExtend(sum, 32);
        if (sum32 + denHalf > std::numeric_limits<int32_t>::max()) ok = false;
        const int64_t prediction = floorShift(sum32 + denHalf, kDenShift);
        const int32_t del = signExtend(int64_t(in[j]) - top - prediction, chanBits);
        res[j] = del;

        int32_t error = del;
        if (del > 0) {
            for (unsigned k = order; k-- > 0;) {
                const int32_t d = top - past[-int(k)];
                const int32_t s = signOf(d);
                coefs[k] = wrap16(coefs[k] - s);
                error -= int32_t(order - k) * int32_t(floorShift(int64_t(s) * d, kDenShift));
                if (error <= 0) break;
            }
        } else if (del < 0) {
            for (unsigned k = order; k-- > 0;) {
                const int32_t d = top - past[-int(k)];
                const int32_t s = signOf(d);
                coefs[k] = wrap16(coefs[k] + s);
                error -= int32_t(order - k) * int32_t(floorShift(-int64_t(s) * d, kDenShift));
                if (error >= 0) break;
            }
        }
    }
    return ok;
}

// --- FrameEncoder ----------------------------------------------------------------

FrameEncoder::FrameEncoder(EncoderOptions options) : options_(options) {
    orders_.push_back(0);
    for (unsigned order : {kCarriedOrder, 8u, 16u})
        if (order <= std::min(options_.maxOrder, kMaxOrder)) orders_.push_back(order);
    for (auto& coefs : carried_) coefs.assign(kCarriedOrder, 0);
}

// Candidates per channel: no prediction; the order-4 coefficients carried
// over from the previous frame (after a few passes over the start of this
// one, as in Apple's encoder: they follow slowly changing signals well and
// cost little); and the least-squares fit of this frame for every order.
// Each is run over the whole frame, adapting as the decoder will, and the
// one with the fewest bits wins.
bool FrameEncoder::chooseOrder(unsigned channel, const int32_t* x, unsigned n, Choice& best) {
    constexpr unsigned chanBits = kBitDepth + 1;  // v = L - R needs one more bit
    bool found = false;
    best.bits = std::numeric_limits<uint64_t>::max();
    auto consider = [&](unsigned order, const std::vector<int16_t>& start, std::vector<int16_t>* adaptedOut) {
        std::vector<int16_t> adapted = start;
        bool ok = predict(x, n, adapted.data(), order, chanBits, scratch_.data());
        bool ambiguous = false;
        const uint64_t bits =
            16 + 16 * uint64_t(order) + adaptiveGolomb(scratch_.data(), n, chanBits, nullptr, &ambiguous);
        if (adaptedOut) *adaptedOut = adapted;
        if (ok && !ambiguous && bits < best.bits) {
            found = true;
            best.order = order;
            best.bits = bits;
            best.coefs = start;
            best.residual.assign(scratch_.begin(), scratch_.begin() + n);
        }
    };
    std::vector<int16_t> start;
    for (unsigned order : orders_) {
        if (order > 0 && order + 1 >= n) continue;
        // Orders above 8 only pay off when order 8 beat the lower ones (tonal
        // music); skipping them otherwise saves time.
        if (order > 8 && (!found || best.order < 8)) continue;
        if (order == kCarriedOrder) {
            std::vector<int16_t>& coefs = carried_[channel];
            const unsigned head = n / 32;
            if (head > order + 1)
                for (unsigned pass = 0; pass < kConvergePasses; ++pass)
                    predict(x, head, coefs.data(), order, chanBits, scratch_.data());
            start = coefs;
            consider(order, start, &coefs);  // carry the adapted coefficients on
        }
        start.assign(order, 0);
        if (order == 0 || leastSquares(x, n, order, start.data())) consider(order, start, nullptr);
    }
    return found;
}

std::vector<uint8_t> FrameEncoder::escape(const int32_t* left, const int32_t* right, unsigned n) {
    const bool partial = n != kFrameLength;
    flac::BitWriter bw;
    bw.writeBits(kChannelPair, 3);
    bw.writeBits(0, 4);   // element instance tag
    bw.writeBits(0, 12);  // unused
    bw.writeBits((partial ? 8u : 0u) | 1u, 4);  // partial frame, no shifted bytes, escape
    if (partial) bw.writeBits(n, 32);
    for (unsigned i = 0; i < n; ++i) {
        bw.writeSigned(left[i], kBitDepth);
        bw.writeSigned(right[i], kBitDepth);
    }
    bw.writeBits(kEnd, 3);
    bw.alignToByte();
    info_ = FrameInfo{true, 0, 0, 0};
    return bw.bytes();
}

std::vector<uint8_t> FrameEncoder::encode(const int32_t* left, const int32_t* right, unsigned n) {
    if (n == 0 || n > kFrameLength) throw std::invalid_argument("ALAC frame size out of range");
    if (!options_.compress) return escape(left, right, n);

    u_.resize(n);
    v_.resize(n);
    scratch_.resize(n);
    constexpr unsigned chanBits = kBitDepth + 1;

    // Stereo matrix: compare the candidates on the start of the frame with the
    // carried-over order-4 coefficients (as Apple's encoder does).
    unsigned mixRes = 0;
    if (options_.stereoDecorrelation) {
        const unsigned head = n >= 512 ? n / 8 : n;
        const unsigned order = orders_.size() > 1 && kCarriedOrder + 1 < head ? kCarriedOrder : 0;
        uint64_t bestBits = std::numeric_limits<uint64_t>::max();
        for (unsigned res = 0; res <= kMaxMixRes; ++res) {
            mix(left, right, head, res, u_.data(), v_.data());
            uint64_t bits = 0;
            for (unsigned ch = 0; ch < 2; ++ch) {
                std::vector<int16_t> coefs = carried_[ch];
                predict(ch ? v_.data() : u_.data(), head, coefs.data(), order, chanBits, scratch_.data());
                bits += adaptiveGolomb(scratch_.data(), head, chanBits, nullptr);
            }
            if (bits < bestBits) {
                bestBits = bits;
                mixRes = res;
            }
        }
    }
    mix(left, right, n, mixRes, u_.data(), v_.data());

    Choice cu, cv;
    const bool partial = n != kFrameLength;
    const uint64_t escapeBits = 7 + 16 + (partial ? 32 : 0) + uint64_t(n) * 2 * kBitDepth + 3;
    if (!chooseOrder(0, u_.data(), n, cu) || !chooseOrder(1, v_.data(), n, cv)) return escape(left, right, n);
    const uint64_t compressedBits = 7 + 16 + (partial ? 32 : 0) + 16 + cu.bits + cv.bits + 3;
    if (compressedBits >= escapeBits) return escape(left, right, n);

    flac::BitWriter bw;
    bw.writeBits(kChannelPair, 3);
    bw.writeBits(0, 4);   // element instance tag
    bw.writeBits(0, 12);  // unused
    bw.writeBits(partial ? 8u : 0u, 4);  // partial frame, no shifted bytes, compressed
    if (partial) bw.writeBits(n, 32);
    bw.writeBits(kMixBits, 8);
    bw.writeBits(mixRes, 8);
    for (const Choice* c : {&cu, &cv}) {
        bw.writeBits(kDenShift, 8);  // prediction mode 0 (high nibble), coefficient shift
        bw.writeBits(kPbFactor << 5 | c->order, 8);
        for (int16_t coef : c->coefs) bw.writeSigned(coef, 16);
    }
    adaptiveGolomb(cu.residual.data(), n, chanBits, &bw);
    adaptiveGolomb(cv.residual.data(), n, chanBits, &bw);
    bw.writeBits(kEnd, 3);
    bw.alignToByte();
    info_ = FrameInfo{false, mixRes, cu.order, cv.order};
    return bw.bytes();
}

}  // namespace cdr::alac
