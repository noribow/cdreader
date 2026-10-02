#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace cdr {

// CRC-32 used in Ogg page headers: polynomial 0x04C11DB7, initial value 0,
// MSB first, no final XOR (unlike the zlib/IEEE CRC in crc32.h).
// Pass the previous result as `crc` to continue a computation.
uint32_t oggCrc32(const uint8_t* data, size_t length, uint32_t crc = 0);

// Packs the packets of one logical bitstream into Ogg pages (RFC 3533).
// Codec independent: an Ogg Opus / Ogg FLAC writer feeds it the codec's
// packets and granule positions. Pages are handed to `sink` as complete byte
// blocks (header, lacing table and body).
class OggStreamWriter {
public:
    using PageSink = std::function<void(const uint8_t* page, size_t bytes)>;

    // A page is emitted once its body reaches `targetPageBytes` (libogg's
    // default is 4096; larger pages mean less overhead but coarser seeking),
    // or when its 255 lacing values are used up.
    static constexpr size_t kTargetPageBytes = 4096;
    static constexpr int64_t kNoGranule = -1;

    OggStreamWriter(uint32_t serialNumber, PageSink sink, size_t targetPageBytes = kTargetPageBytes)
        : serial_(serialNumber), sink_(std::move(sink)), targetPageBytes_(targetPageBytes) {}

    // Appends a packet. `granulePosition` is the codec's position at the end
    // of this packet; it is written on the page where the packet ends.
    // With `endOfStream` the packet is the last one: it is flushed with the
    // EOS flag and no further packets are accepted.
    void writePacket(const uint8_t* data, size_t size, int64_t granulePosition, bool endOfStream = false);
    void writePacket(const std::vector<uint8_t>& packet, int64_t granulePosition, bool endOfStream = false) {
        writePacket(packet.data(), packet.size(), granulePosition, endOfStream);
    }

    // Emits the buffered data as a page now, e.g. so that codec headers end
    // their page as Ogg Opus / Ogg FLAC require. No-op when nothing is buffered.
    void flush() { emitPage(false); }

    uint32_t pagesWritten() const { return sequence_; }
    bool finished() const { return finished_; }

private:
    void emitPage(bool endOfStream);

    uint32_t serial_;
    PageSink sink_;
    size_t targetPageBytes_;
    uint32_t sequence_ = 0;
    std::vector<uint8_t> lacing_;
    std::vector<uint8_t> body_;
    int64_t granule_ = kNoGranule;   // granule of the last packet completed on the pending page
    bool continued_ = false;         // the pending page starts with the rest of a packet
    bool finished_ = false;
};

}  // namespace cdr
