#include "flac_decoder.h"

#include <stdexcept>

#include "cdreader/flac_encoder.h"

namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t bits(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (pos_ / 8 >= size_) throw std::runtime_error("unexpected end of FLAC data");
            v = (v << 1) | ((data_[pos_ / 8] >> (7 - pos_ % 8)) & 1);
            ++pos_;
        }
        return v;
    }
    int32_t signedBits(unsigned n) {
        if (n == 0) return 0;
        const uint32_t v = bits(n);
        return n < 32 && (v >> (n - 1)) ? int32_t(int64_t(v) - (int64_t(1) << n)) : int32_t(v);
    }
    uint32_t unary() {
        uint32_t n = 0;
        while (bits(1) == 0) ++n;
        return n;
    }
    void align() { pos_ = (pos_ + 7) / 8 * 8; }
    size_t bytePos() const { return pos_ / 8; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void residual(BitReader& br, unsigned n, unsigned order, std::vector<int64_t>& out) {
    const unsigned method = br.bits(2);
    if (method > 1) throw std::runtime_error("reserved residual coding method");
    const unsigned paramBits = method ? 5 : 4;
    const unsigned po = br.bits(4);
    if ((n >> po) << po != n || (n >> po) < order) throw std::runtime_error("bad partition order");
    for (unsigned p = 0; p < (1u << po); ++p) {
        const unsigned count = (n >> po) - (p == 0 ? order : 0);
        const unsigned k = br.bits(paramBits);
        if (k == (1u << paramBits) - 1) {
            const unsigned raw = br.bits(5);
            for (unsigned i = 0; i < count; ++i) out.push_back(br.signedBits(raw));
        } else {
            for (unsigned i = 0; i < count; ++i) {
                const uint64_t u = (uint64_t(br.unary()) << k) | br.bits(k);
                out.push_back((u & 1) ? -int64_t(u >> 1) - 1 : int64_t(u >> 1));
            }
        }
    }
}

std::vector<int64_t> subframe(BitReader& br, unsigned n, unsigned bps) {
    if (br.bits(1)) throw std::runtime_error("subframe padding bit set");
    const unsigned type = br.bits(6);
    unsigned wasted = 0;
    if (br.bits(1)) wasted = br.unary() + 1;
    bps -= wasted;
    std::vector<int64_t> x;
    if (type == 0) {
        x.assign(n, br.signedBits(bps));
    } else if (type == 1) {
        for (unsigned i = 0; i < n; ++i) x.push_back(br.signedBits(bps));
    } else if (type >= 8 && type <= 12) {
        const unsigned order = type - 8;
        for (unsigned i = 0; i < order; ++i) x.push_back(br.signedBits(bps));
        std::vector<int64_t> res;
        residual(br, n, order, res);
        static const int coef[5][4] = {{0}, {1}, {2, -1}, {3, -3, 1}, {4, -6, 4, -1}};
        for (unsigned i = order; i < n; ++i) {
            int64_t p = 0;
            for (unsigned j = 0; j < order; ++j) p += coef[order][j] * x[i - 1 - j];
            x.push_back(p + res[i - order]);
        }
    } else if (type >= 32) {
        const unsigned order = type - 31;
        for (unsigned i = 0; i < order; ++i) x.push_back(br.signedBits(bps));
        const unsigned precision = br.bits(4) + 1;
        if (precision == 16) throw std::runtime_error("invalid LPC precision");
        const int shift = br.signedBits(5);
        if (shift < 0) throw std::runtime_error("negative LPC shift");
        std::vector<int64_t> q;
        for (unsigned i = 0; i < order; ++i) q.push_back(br.signedBits(precision));
        std::vector<int64_t> res;
        residual(br, n, order, res);
        for (unsigned i = order; i < n; ++i) {
            int64_t p = 0;
            for (unsigned j = 0; j < order; ++j) p += q[j] * x[i - 1 - j];
            x.push_back((p >> shift) + res[i - order]);
        }
    } else {
        throw std::runtime_error("reserved subframe type");
    }
    for (int64_t& v : x) v *= int64_t(1) << wasted;
    return x;
}

}  // namespace

DecodedFlac decodeFlac(const std::vector<uint8_t>& f) {
    DecodedFlac d;
    if (f.size() < 4 || f[0] != 'f' || f[1] != 'L' || f[2] != 'a' || f[3] != 'C')
        throw std::runtime_error("missing fLaC marker");
    size_t pos = 4;
    for (bool last = false; !last;) {
        if (pos + 4 > f.size()) throw std::runtime_error("truncated metadata");
        last = (f[pos] & 0x80) != 0;
        const int type = f[pos] & 0x7F;
        const size_t len = size_t(be(&f[pos + 1], 3));
        pos += 4;
        if (pos + len > f.size()) throw std::runtime_error("truncated metadata block");
        const uint8_t* b = &f[pos];
        d.blockTypes.push_back(type);
        if (type == 0) {
            if (len != 34) throw std::runtime_error("bad STREAMINFO length");
            d.minBlockSize = unsigned(be(b, 2));
            d.maxBlockSize = unsigned(be(b + 2, 2));
            d.minFrameSize = uint32_t(be(b + 4, 3));
            d.maxFrameSize = uint32_t(be(b + 7, 3));
            const uint64_t v = be(b + 10, 8);
            d.sampleRate = uint32_t(v >> 44);
            d.channels = unsigned((v >> 41) & 7) + 1;
            d.bitsPerSample = unsigned((v >> 36) & 31) + 1;
            d.totalSamples = v & ((uint64_t(1) << 36) - 1);
            for (int i = 0; i < 16; ++i) d.md5[size_t(i)] = b[18 + i];
        } else if (type == 3) {
            for (size_t o = 0; o + 18 <= len; o += 18)
                d.seekPoints.push_back({be(b + o, 8), be(b + o + 8, 8), unsigned(be(b + o + 16, 2))});
        } else if (type == 4) {
            size_t o = 0;
            const uint32_t vl = le32(b);
            d.vendor.assign(reinterpret_cast<const char*>(b + 4), vl);
            o = 4 + vl;
            const uint32_t count = le32(b + o);
            o += 4;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t cl = le32(b + o);
                d.comments.emplace_back(reinterpret_cast<const char*>(b + o + 4), cl);
                o += 4 + cl;
            }
            if (o != len) throw std::runtime_error("bad VORBIS_COMMENT length");
        }
        pos += len;
    }
    if (d.blockTypes.empty() || d.blockTypes[0] != 0) throw std::runtime_error("STREAMINFO is not first");

    const size_t firstFrame = pos;
    d.firstFrame = pos;
    while (pos < f.size()) {
        d.frameOffsets.push_back(pos - firstFrame);
        BitReader br(&f[pos], f.size() - pos);
        if (br.bits(16) != 0xFFF8) throw std::runtime_error("bad frame sync");
        const unsigned bsCode = br.bits(4);
        if (br.bits(4) != 9) throw std::runtime_error("unexpected sample rate code");
        const unsigned assignment = br.bits(4);
        if (br.bits(3) != 4 || br.bits(1) != 0) throw std::runtime_error("unexpected sample size");
        // UTF-8 frame number
        uint32_t first = br.bits(8), number = 0;
        if (first < 0x80) {
            number = first;
        } else {
            unsigned extra = 0;
            while (first & (0x40 >> extra)) ++extra;
            number = first & (0x3F >> extra);
            for (unsigned i = 0; i < extra; ++i) {
                const uint32_t c = br.bits(8);
                if ((c & 0xC0) != 0x80) throw std::runtime_error("bad UTF-8 frame number");
                number = (number << 6) | (c & 0x3F);
            }
        }
        if (number != d.frames) throw std::runtime_error("unexpected frame number");
        unsigned n;
        if (bsCode == 6) n = br.bits(8) + 1;
        else if (bsCode == 7) n = br.bits(16) + 1;
        else if (bsCode >= 8) n = 256u << (bsCode - 8);
        else throw std::runtime_error("unexpected block size code");
        const size_t headerBytes = br.bytePos();
        if (br.bits(8) != cdr::flac::crc8(&f[pos], headerBytes)) throw std::runtime_error("frame header CRC mismatch");

        std::vector<int64_t> a, b;
        if (assignment == 1) {
            a = subframe(br, n, 16);
            b = subframe(br, n, 16);
        } else if (assignment == 8) {
            a = subframe(br, n, 16);
            b = subframe(br, n, 17);
            for (unsigned i = 0; i < n; ++i) b[i] = a[i] - b[i];
        } else if (assignment == 9) {
            a = subframe(br, n, 17);
            b = subframe(br, n, 16);
            for (unsigned i = 0; i < n; ++i) a[i] += b[i];
        } else if (assignment == 10) {
            a = subframe(br, n, 16);
            b = subframe(br, n, 17);
            for (unsigned i = 0; i < n; ++i) {
                const int64_t mid = a[i] * 2 + (b[i] & 1), side = b[i];
                a[i] = (mid + side) / 2;
                b[i] = (mid - side) / 2;
            }
        } else {
            throw std::runtime_error("unexpected channel assignment");
        }
        br.align();
        const size_t bodyBytes = br.bytePos();
        if (br.bits(16) != cdr::flac::crc16(&f[pos], bodyBytes)) throw std::runtime_error("frame CRC-16 mismatch");
        for (unsigned i = 0; i < n; ++i) {
            for (int64_t v : {a[i], b[i]}) {
                if (v < -32768 || v > 32767) throw std::runtime_error("decoded sample out of range");
                d.pcm.push_back(uint8_t(uint16_t(v)));
                d.pcm.push_back(uint8_t(uint16_t(v) >> 8));
            }
        }
        ++d.frames;
        pos += bodyBytes + 2;
    }
    return d;
}
