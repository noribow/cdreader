#include "cdreader/crc32.h"

#include <array>

namespace cdr {

static const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void Crc32::update(const uint8_t* data, size_t length) {
    const auto& table = crcTable();
    uint32_t c = state_;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    state_ = c;
}

}  // namespace cdr
