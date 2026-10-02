#include "alac_decoder.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

// --- Box tree ---------------------------------------------------------------

// Boxes holding only boxes, and how many bytes precede the children.
const std::map<std::string, unsigned>& containers() {
    static const std::map<std::string, unsigned> m = {
        {"moov", 0}, {"trak", 0}, {"mdia", 0}, {"minf", 0}, {"dinf", 0}, {"stbl", 0}, {"udta", 0},
        {"ilst", 0}, {"meta", 4},  // full box
        {"dref", 8},               // full box + entry count
        {"stsd", 8},               // full box + entry count
        {"alac", 0},  // see parseChildren: the sample entry (28 bytes) vs the cookie box
    };
    return m;
}

void parseChildren(const std::vector<uint8_t>& f, Mp4Box& parent, uint64_t begin, uint64_t end, bool inIlst,
                   bool inStsd) {
    uint64_t pos = begin;
    while (pos < end) {
        if (end - pos < 8) fail("truncated box header in " + parent.type);
        Mp4Box b;
        b.offset = pos;
        b.size = be(&f[pos], 4);
        b.type.assign(reinterpret_cast<const char*>(&f[pos + 4]), 4);
        b.headerSize = 8;
        if (b.size == 1) {
            if (end - pos < 16) fail("truncated 64-bit box header");
            b.size = be(&f[pos + 8], 8);
            b.headerSize = 16;
        } else if (b.size == 0) {
            b.size = end - pos;  // to the end of the file
        }
        if (b.size < b.headerSize || b.size > end - pos) fail("box '" + b.type + "' exceeds its parent");
        const uint64_t bodyBegin = pos + b.headerSize, bodyEnd = pos + b.size;
        auto it = containers().find(b.type);
        if (inIlst) {
            // Every ilst item holds boxes (data, or mean / name / data for "----").
            parseChildren(f, b, bodyBegin, bodyEnd, false, false);
        } else if (b.type == "alac" && inStsd) {
            if (b.size < b.headerSize + 28) fail("short alac sample entry");
            parseChildren(f, b, bodyBegin + 28, bodyEnd, false, false);
        } else if (it != containers().end() && b.type != "alac") {
            parseChildren(f, b, bodyBegin + it->second, bodyEnd, b.type == "ilst", b.type == "stsd");
        }
        parent.children.push_back(std::move(b));
        pos = bodyEnd;
    }
    if (pos != end) fail("boxes do not fill " + parent.type);
}

const uint8_t* body(const std::vector<uint8_t>& f, const Mp4Box& b) { return f.data() + b.offset + b.headerSize; }
uint64_t bodySize(const Mp4Box& b) { return b.size - b.headerSize; }

const Mp4Box& need(const Mp4Box& root, const char* path) {
    const Mp4Box* b = root.find(path);
    if (!b) fail(std::string("missing box ") + path);
    return *b;
}

// Version / flags of a full box, then a cursor over the rest.
struct Reader {
    const uint8_t* p;
    uint64_t left;
    uint64_t u(int n) {
        if (left < uint64_t(n)) fail("box too short");
        const uint64_t v = be(p, n);
        p += n;
        left -= uint64_t(n);
        return v;
    }
};

Reader fullBody(const std::vector<uint8_t>& f, const Mp4Box& b, unsigned* version = nullptr) {
    Reader r{body(f, b), bodySize(b)};
    const unsigned v = unsigned(r.u(1));
    r.u(3);
    if (version) *version = v;
    return r;
}

// --- ALAC bitstream ----------------------------------------------------------

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint32_t bits(unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | peekBit(pos_ + i);
        pos_ += n;
        if (pos_ > size_ * 8) fail("ALAC frame overrun");
        return v;
    }
    uint32_t peek(unsigned n) const {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | peekBit(pos_ + i);
        return v;
    }
    void skip(unsigned n) {
        pos_ += n;
        if (pos_ > size_ * 8) fail("ALAC frame overrun");
    }
    void align() { pos_ = (pos_ + 7) / 8 * 8; }
    size_t bitPos() const { return pos_; }

private:
    uint32_t peekBit(size_t pos) const { return pos / 8 < size_ ? (data_[pos / 8] >> (7 - pos % 8)) & 1 : 0; }
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

int32_t signExtend(int64_t v, unsigned bits) {
    const uint64_t u = uint64_t(v) & ((uint64_t(1) << bits) - 1);
    return int32_t(u >> (bits - 1) ? int64_t(u) - (int64_t(1) << bits) : int64_t(u));
}

int64_t floorShift(int64_t v, unsigned s) { return v >= 0 ? v >> s : -((-v - 1) >> s) - 1; }

unsigned lead(uint32_t v) {  // leading zeros of a 32-bit value
    unsigned n = 0;
    for (uint32_t c = 0x80000000u; c && !(v & c); c >>= 1) ++n;
    return n;
}

// dyn_get / dyn_get_32bit of the reference decoder.
uint32_t golomb(BitReader& br, uint32_t m, unsigned k, unsigned escapeBits) {
    unsigned ones = 0;
    while (ones < 9 && br.peek(ones + 1) == (1u << (ones + 1)) - 1) ++ones;
    if (ones >= 9) {
        br.skip(9);
        return br.bits(escapeBits);
    }
    br.skip(ones + 1);  // the 1-bits and the terminating 0
    uint32_t result = ones;
    if (k != 1) {
        const uint32_t v = br.peek(k);
        br.skip(k - 1);
        result *= m;
        if (v >= 2) {
            result += v - 1;
            br.skip(1);
        }
    }
    return result;
}

void dynDecomp(BitReader& br, unsigned pb, unsigned mb, unsigned kb, unsigned n, unsigned bitSize,
               std::vector<int32_t>& out) {
    out.assign(n, 0);
    uint32_t history = mb;
    uint32_t zmode = 0;
    unsigned c = 0;
    const uint32_t wb = (1u << kb) - 1;
    while (c < n) {
        unsigned k = 31 - lead((history >> 9) + 3);
        k = std::min(k, kb);
        const uint32_t value = golomb(br, (1u << k) - 1, k, bitSize);
        const uint32_t nd = value + zmode;
        const int64_t magnitude = (int64_t(nd) + 1) >> 1;
        out[c++] = int32_t((nd & 1) ? -magnitude : magnitude);
        history = pb * (value + zmode) + history - ((pb * history) >> 9);
        if (value > 0xFFFF) history = 0xFFFF;
        zmode = 0;
        if ((history << 2) < 512 && c < n) {
            zmode = 1;
            const unsigned kz = lead(history) - 24 + ((history + 16) >> 6);
            const uint32_t zeros = golomb(br, ((1u << kz) - 1) & wb, kz, 16);
            if (c + zeros > n) fail("zero run beyond the end of the frame");
            c += zeros;  // already zero
            if (zeros >= 65535) zmode = 0;
            history = 0;
        }
    }
}

int16_t wrap16(int32_t v) { return int16_t(signExtend(v, 16)); }
int32_t signOf(int32_t v) { return (v > 0) - (v < 0); }

// unpc_block with 32-bit wrap-around sums, as in the reference decoder.
void unpredict(const std::vector<int32_t>& pc, std::vector<int32_t>& out, std::vector<int16_t> coefs, unsigned order,
               unsigned chanBits, unsigned denShift, unsigned mode) {
    const unsigned n = unsigned(pc.size());
    out.assign(n, 0);
    if (n == 0) return;
    if (mode != 0) fail("prediction mode " + std::to_string(mode) + " not produced by the encoder");
    if (denShift == 0) fail("coefficient shift 0");
    out[0] = pc[0];
    if (order == 0) {
        std::copy(pc.begin(), pc.end(), out.begin());
        return;
    }
    if (order == 31) fail("first-order mode not produced by the encoder");
    for (unsigned j = 1; j <= order && j < n; ++j) out[j] = signExtend(int64_t(pc[j]) + out[j - 1], chanBits);
    const int32_t denHalf = 1 << (denShift - 1);
    for (unsigned j = order + 1; j < n; ++j) {
        const int32_t top = out[j - order - 1];
        uint32_t sum = 0;  // int32 arithmetic of the reference, wrapping around
        for (unsigned k = 0; k < order; ++k)
            sum += uint32_t(int64_t(coefs[k]) * (int64_t(out[j - 1 - k]) - top));
        const int32_t rounded = signExtend(int64_t(sum) + denHalf, 32);
        const int32_t del0 = pc[j];
        out[j] = signExtend(int64_t(del0) + top + floorShift(rounded, denShift), chanBits);
        int32_t error = del0;
        if (del0 > 0) {
            for (unsigned k = order; k-- > 0;) {
                const int32_t d = top - out[j - 1 - k];
                const int32_t s = signOf(d);
                coefs[k] = wrap16(coefs[k] - s);
                error -= int32_t(order - k) * int32_t(floorShift(int64_t(s) * d, denShift));
                if (error <= 0) break;
            }
        } else if (del0 < 0) {
            for (unsigned k = order; k-- > 0;) {
                const int32_t d = top - out[j - 1 - k];
                const int32_t s = signOf(d);
                coefs[k] = wrap16(coefs[k] + s);
                error -= int32_t(order - k) * int32_t(floorShift(-int64_t(s) * d, denShift));
                if (error >= 0) break;
            }
        }
    }
}

void put16(std::vector<uint8_t>& pcm, int32_t v) {
    if (v < -32768 || v > 32767) fail("decoded sample out of 16-bit range");
    pcm.push_back(uint8_t(uint32_t(v)));
    pcm.push_back(uint8_t(uint32_t(v) >> 8));
}

}  // namespace

const Mp4Box* Mp4Box::child(const std::string& t) const {
    for (const Mp4Box& c : children)
        if (c.type == t) return &c;
    return nullptr;
}

const Mp4Box* Mp4Box::find(const std::string& path) const {
    const Mp4Box* b = this;
    size_t pos = 0;
    while (b && pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        b = b->child(part);
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return b;
}

AlacFrameInfo decodeAlacFrame(const uint8_t* data, size_t size, uint32_t frameLength, unsigned pb, unsigned mb,
                              unsigned kb, std::vector<uint8_t>& pcm) {
    BitReader br(data, size);
    AlacFrameInfo info;
    if (br.bits(3) != 1) fail("expected a channel pair element");
    if (br.bits(4) != 0) fail("element instance tag");
    if (br.bits(12) != 0) fail("unused header bits set");
    const unsigned flags = br.bits(4);
    info.partial = flags >> 3;
    const unsigned bytesShifted = (flags >> 1) & 3;
    info.escaped = flags & 1;
    if (bytesShifted != 0) fail("shifted bytes in a 16-bit stream");
    info.samples = frameLength;
    if (info.partial) {
        info.samples = br.bits(32);
        if (info.samples == 0 || info.samples > frameLength) fail("bad partial frame length");
    }
    const unsigned n = info.samples;
    std::vector<int32_t> u(n), v(n);
    if (info.escaped) {
        for (unsigned i = 0; i < n; ++i) {
            u[i] = signExtend(br.bits(16), 16);
            v[i] = signExtend(br.bits(16), 16);
        }
    } else {
        constexpr unsigned chanBits = 17;
        info.mixBits = br.bits(8);
        info.mixRes = br.bits(8);
        std::vector<int16_t> coefsU, coefsV;
        unsigned b = br.bits(8);
        info.modeU = b >> 4;
        info.denShiftU = b & 15;
        b = br.bits(8);
        info.pbFactorU = b >> 5;
        info.orderU = b & 31;
        for (unsigned i = 0; i < info.orderU; ++i) coefsU.push_back(int16_t(signExtend(br.bits(16), 16)));
        b = br.bits(8);
        info.modeV = b >> 4;
        info.denShiftV = b & 15;
        b = br.bits(8);
        info.pbFactorV = b >> 5;
        info.orderV = b & 31;
        for (unsigned i = 0; i < info.orderV; ++i) coefsV.push_back(int16_t(signExtend(br.bits(16), 16)));
        std::vector<int32_t> pc;
        dynDecomp(br, pb * info.pbFactorU / 4, mb, kb, n, chanBits, pc);
        unpredict(pc, u, coefsU, info.orderU, chanBits, info.denShiftU, info.modeU);
        dynDecomp(br, pb * info.pbFactorV / 4, mb, kb, n, chanBits, pc);
        unpredict(pc, v, coefsV, info.orderV, chanBits, info.denShiftV, info.modeV);
    }
    if (br.bits(3) != 7) fail("expected the end tag");
    br.align();
    if (br.bitPos() != size * 8) fail("frame has trailing bytes");

    for (unsigned i = 0; i < n; ++i) {
        int32_t l = u[i], r = v[i];
        if (info.mixRes != 0) {
            l = u[i] + v[i] - int32_t(floorShift(int64_t(info.mixRes) * v[i], info.mixBits));
            r = l - v[i];
        }
        put16(pcm, l);
        put16(pcm, r);
    }
    return info;
}

DecodedM4a decodeM4a(const std::vector<uint8_t>& f) {
    DecodedM4a d;
    d.root.size = f.size();
    parseChildren(f, d.root, 0, f.size(), false, false);

    // ftyp
    const Mp4Box& ftyp = need(d.root, "ftyp");
    if (d.root.children.empty() || d.root.children[0].type != "ftyp") fail("ftyp is not the first box");
    {
        const uint8_t* p = body(f, ftyp);
        const uint64_t n = bodySize(ftyp);
        if (n < 8 || n % 4) fail("bad ftyp");
        d.majorBrand.assign(reinterpret_cast<const char*>(p), 4);
        for (uint64_t i = 8; i < n; i += 4) d.compatibleBrands.emplace_back(reinterpret_cast<const char*>(p + i), 4);
    }
    const Mp4Box& mdat = need(d.root, "mdat");
    d.mdatDataOffset = mdat.offset + mdat.headerSize;
    d.mdatDataSize = bodySize(mdat);

    unsigned version = 0;
    {
        Reader r = fullBody(f, need(d.root, "moov/mvhd"), &version);
        r.u(version ? 16 : 8);
        d.movieTimescale = uint32_t(r.u(4));
        d.movieDuration = r.u(version ? 8 : 4);
    }
    {
        Reader r = fullBody(f, need(d.root, "moov/trak/tkhd"), &version);
        r.u(version ? 16 : 8);
        if (r.u(4) != 1) fail("track ID");
        r.u(4);
        d.trackDuration = r.u(version ? 8 : 4);
    }
    {
        Reader r = fullBody(f, need(d.root, "moov/trak/mdia/mdhd"), &version);
        r.u(version ? 16 : 8);
        d.mediaTimescale = uint32_t(r.u(4));
        d.mediaDuration = r.u(version ? 8 : 4);
    }
    {
        Reader r = fullBody(f, need(d.root, "moov/trak/mdia/hdlr"));
        r.u(4);
        const uint64_t t = r.u(4);
        d.handlerType = {char(t >> 24), char(t >> 16), char(t >> 8), char(t)};
    }
    need(d.root, "moov/trak/mdia/minf/smhd");
    need(d.root, "moov/trak/mdia/minf/dinf/dref/url ");

    // Sample description: one 'alac' entry with its cookie box.
    const Mp4Box& stbl = need(d.root, "moov/trak/mdia/minf/stbl");
    const Mp4Box& stsd = need(stbl, "stsd");
    if (be(body(f, stsd) + 4, 4) != 1 || stsd.children.size() != 1) fail("expected one sample entry");
    const Mp4Box& entry = stsd.children[0];
    d.sampleEntryType = entry.type;
    {
        const uint8_t* p = body(f, entry);
        if (be(p + 6, 2) != 1) fail("data reference index");
        d.entryChannels = unsigned(be(p + 16, 2));
        d.entrySampleSize = unsigned(be(p + 18, 2));
        d.entrySampleRate = uint32_t(be(p + 24, 4) >> 16);
    }
    const Mp4Box& cookieBox = need(entry, "alac");
    if (cookieBox.size != 36) fail("alac cookie box is not 36 bytes");
    {
        Reader r = fullBody(f, cookieBox);
        d.cookie.assign(r.p, r.p + 24);
        d.frameLength = uint32_t(r.u(4));
        d.compatibleVersion = unsigned(r.u(1));
        d.bitDepth = unsigned(r.u(1));
        d.pb = unsigned(r.u(1));
        d.mb = unsigned(r.u(1));
        d.kb = unsigned(r.u(1));
        d.channels = unsigned(r.u(1));
        d.maxRun = unsigned(r.u(2));
        d.maxFrameBytes = uint32_t(r.u(4));
        d.avgBitRate = uint32_t(r.u(4));
        d.sampleRate = uint32_t(r.u(4));
    }
    if (d.bitDepth != 16 || d.channels != 2) fail("decoder only handles 16-bit stereo");

    {
        Reader r = fullBody(f, need(stbl, "stts"));
        for (uint64_t n = r.u(4); n-- > 0;) {
            const uint32_t count = uint32_t(r.u(4));
            d.stts.emplace_back(count, uint32_t(r.u(4)));
        }
        if (r.left) fail("stts size");
    }
    {
        Reader r = fullBody(f, need(stbl, "stsc"));
        for (uint64_t n = r.u(4); n-- > 0;) {
            std::array<uint32_t, 3> e{};
            for (uint32_t& x : e) x = uint32_t(r.u(4));
            d.stsc.push_back(e);
        }
        if (r.left) fail("stsc size");
    }
    {
        Reader r = fullBody(f, need(stbl, "stsz"));
        const uint32_t fixed = uint32_t(r.u(4));
        d.stszSampleSize = fixed;
        const uint64_t count = r.u(4);
        for (uint64_t i = 0; i < count; ++i) d.sampleSizes.push_back(fixed ? fixed : uint32_t(r.u(4)));
        if (r.left) fail("stsz size");
    }
    {
        const Mp4Box* stco = stbl.child("stco");
        const Mp4Box* co64 = stbl.child("co64");
        if (!stco == !co64) fail("expected one of stco / co64");
        d.co64 = co64 != nullptr;
        Reader r = fullBody(f, d.co64 ? *co64 : *stco);
        for (uint64_t n = r.u(4); n-- > 0;) d.chunkOffsets.push_back(r.u(d.co64 ? 8 : 4));
        if (r.left) fail("chunk offset table size");
    }

    // Resolve the frame offsets: chunks, their frame counts from stsc, sizes from stsz.
    size_t frame = 0;
    for (size_t c = 0; c < d.chunkOffsets.size(); ++c) {
        uint32_t perChunk = 0;
        for (const auto& e : d.stsc)
            if (e[0] <= c + 1) perChunk = e[1];
        if (perChunk == 0) fail("chunk without stsc entry");
        uint64_t offset = d.chunkOffsets[c];
        for (uint32_t i = 0; i < perChunk; ++i, ++frame) {
            if (frame >= d.sampleSizes.size()) fail("stsc describes more frames than stsz");
            d.frameOffsets.push_back(offset);
            offset += d.sampleSizes[frame];
        }
    }
    if (frame != d.sampleSizes.size()) fail("stsc describes fewer frames than stsz");

    // iTunes tags.
    if (const Mp4Box* meta = d.root.find("moov/udta/meta")) {
        Reader r = fullBody(f, need(*meta, "hdlr"));
        r.u(4);
        const uint64_t t = r.u(4);
        d.metaHandler = {char(t >> 24), char(t >> 16), char(t >> 8), char(t)};
        for (const Mp4Box& item : need(*meta, "ilst").children) {
            const Mp4Box& data = need(item, "data");
            const uint8_t* p = body(f, data);
            const uint64_t n = bodySize(data);
            if (n < 8) fail("short data box");
            const uint32_t type = uint32_t(be(p, 4));
            std::string value(reinterpret_cast<const char*>(p + 8), size_t(n - 8));
            std::string key = item.type;
            if (item.type == "trkn") {
                if (type != 0 || n != 16) fail("trkn layout");
                value = std::to_string(be(p + 10, 2)) + "/" + std::to_string(be(p + 12, 2));
            } else if (item.type == "----") {
                const Mp4Box& mean = need(item, "mean");
                const Mp4Box& name = need(item, "name");
                key = "----:" + std::string(reinterpret_cast<const char*>(body(f, mean) + 4), size_t(bodySize(mean) - 4)) +
                      ":" + std::string(reinterpret_cast<const char*>(body(f, name) + 4), size_t(bodySize(name) - 4));
                if (type != 1) fail("freeform data is not UTF-8");
            } else if (type != 1) {
                fail("text item " + item.type + " is not UTF-8");
            }
            if (d.tags.count(key)) fail("duplicate tag " + key);
            d.tags[key] = value;
        }
    }

    // Frames: within mdat, decoded lengths as stts says.
    std::vector<uint32_t> durations;
    for (const auto& [count, duration] : d.stts) durations.insert(durations.end(), count, duration);
    if (durations.size() != d.sampleSizes.size()) fail("stts and stsz differ in frame count");
    for (size_t i = 0; i < d.sampleSizes.size(); ++i) {
        const uint64_t off = d.frameOffsets[i];
        if (off < d.mdatDataOffset || off + d.sampleSizes[i] > d.mdatDataOffset + d.mdatDataSize)
            fail("frame outside mdat");
        const AlacFrameInfo info =
            decodeAlacFrame(f.data() + off, d.sampleSizes[i], d.frameLength, d.pb, d.mb, d.kb, d.pcm);
        if (info.samples != durations[i]) fail("frame length differs from stts");
        d.frames.push_back(info);
    }
    return d;
}
