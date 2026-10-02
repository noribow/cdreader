#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cdr {

enum class DataDirection { None, In, Out };

// Decoded fixed-format sense data (SPC-3 4.5.3).
struct SenseInfo {
    uint8_t key = 0;
    uint8_t asc = 0;
    uint8_t ascq = 0;
};

struct ScsiResult {
    bool transportOk = false;  // the request reached the device and completed
    uint8_t status = 0;        // SCSI status byte: 0x00 GOOD, 0x02 CHECK CONDITION
    SenseInfo sense;
    size_t transferred = 0;    // bytes actually moved by a data-in/out command
    std::string error;         // transport-level error description

    bool ok() const { return transportOk && status == 0; }
    std::string describe() const;
};

// Extracts key/ASC/ASCQ from a raw sense buffer (fixed or descriptor format).
SenseInfo parseSense(const uint8_t* sense, size_t length);

// Sends SCSI/MMC commands to an optical drive. Implemented per platform:
// Windows uses SCSI pass-through (SPTI); Android will use USB mass storage
// bulk-only transport. Everything above this interface is shared.
class ScsiTransport {
public:
    virtual ~ScsiTransport() = default;

    virtual ScsiResult execute(const uint8_t* cdb, size_t cdbLength,
                               void* data, size_t dataLength,
                               DataDirection direction,
                               unsigned timeoutSeconds) = 0;
};

}  // namespace cdr
