#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cdr {

// Incremental MD5 (RFC 1321), used for the FLAC STREAMINFO audio signature.
class Md5 {
public:
    Md5();
    void update(const uint8_t* data, size_t length);
    std::array<uint8_t, 16> finish();  // the object must not be updated afterwards

private:
    void transform(const uint8_t* block);

    uint32_t state_[4];
    uint64_t length_ = 0;  // bytes hashed so far
    uint8_t buffer_[64];
};

}  // namespace cdr
