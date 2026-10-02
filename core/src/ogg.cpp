#include "cdreader/ogg.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace cdr {

namespace {

const std::array<uint32_t, 256>& oggCrcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

constexpr size_t kHeaderBytes = 27;
constexpr size_t kMaxSegments = 255;

}  // namespace

uint32_t oggCrc32(const uint8_t* data, size_t length, uint32_t crc) {
    const auto& table = oggCrcTable();
    for (size_t i = 0; i < length; ++i) crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xFF];
    return crc;
}

// Lacing: a packet is stored as segments of 255 bytes followed by one
// segment shorter than 255 (possibly 0 bytes) that marks its end. A packet
// longer than the room left on a page continues on the next page, which then
// carries the "continued" flag; a page on which no packet ends has granule -1.
void OggStreamWriter::writePacket(const uint8_t* data, size_t size, int64_t granulePosition, bool endOfStream) {
    if (finished_) throw std::logic_error("Ogg stream already ended");
    size_t offset = 0;
    for (;;) {
        const size_t segment = size - offset < 255 ? size - offset : 255;
        lacing_.push_back(uint8_t(segment));
        body_.insert(body_.end(), data + offset, data + offset + segment);
        offset += segment;
        const bool packetDone = segment < 255;
        if (packetDone) granule_ = granulePosition;
        if (lacing_.size() == kMaxSegments) {
            emitPage(packetDone && endOfStream);
            continued_ = !packetDone;
        }
        if (packetDone) break;
    }
    if (endOfStream) {
        emitPage(true);  // no-op when the packet just filled a page that already carries EOS
        finished_ = true;
    } else if (body_.size() >= targetPageBytes_) {
        emitPage(false);
    }
}

void OggStreamWriter::emitPage(bool endOfStream) {
    if (lacing_.empty()) return;
    std::vector<uint8_t> page(kHeaderBytes + lacing_.size() + body_.size());
    uint8_t* h = page.data();
    h[0] = 'O';
    h[1] = 'g';
    h[2] = 'g';
    h[3] = 'S';
    h[4] = 0;  // stream structure version
    h[5] = uint8_t((continued_ ? 0x01 : 0) | (sequence_ == 0 ? 0x02 : 0) | (endOfStream ? 0x04 : 0));
    const uint64_t granule = uint64_t(granule_);  // -1 becomes all ones, as required
    for (int i = 0; i < 8; ++i) h[6 + i] = uint8_t(granule >> (8 * i));
    put32(h + 14, serial_);
    put32(h + 18, sequence_);
    // h[22..25]: CRC, computed over the whole page with this field zero
    h[26] = uint8_t(lacing_.size());
    std::copy(lacing_.begin(), lacing_.end(), page.begin() + ptrdiff_t(kHeaderBytes));
    std::copy(body_.begin(), body_.end(), page.begin() + ptrdiff_t(kHeaderBytes + lacing_.size()));
    put32(h + 22, oggCrc32(page.data(), page.size()));

    ++sequence_;
    lacing_.clear();
    body_.clear();
    granule_ = kNoGranule;
    continued_ = false;
    sink_(page.data(), page.size());
}

}  // namespace cdr
