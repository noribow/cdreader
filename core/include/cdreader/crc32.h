#pragma once

#include <cstddef>
#include <cstdint>

namespace cdr {

// Incremental CRC-32 (IEEE 802.3), as printed in EAC-style rip logs.
class Crc32 {
public:
    void update(const uint8_t* data, size_t length);
    uint32_t value() const { return ~state_; }

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

}  // namespace cdr
