#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cdreader/scsi.h"
#include "cdreader/toc.h"

namespace cdr {

// 26 * 2352 = 61,152 bytes: stays below the common 64 KiB transfer limit.
constexpr uint32_t kMaxSectorsPerRead = 26;

struct DriveInfo {
    std::string vendor;
    std::string product;
    std::string revision;

    std::string displayName() const;
};

class ScsiError : public std::runtime_error {
public:
    ScsiError(const std::string& what, ScsiResult result)
        : std::runtime_error(what + ": " + result.describe()), result_(std::move(result)) {}
    const ScsiResult& result() const { return result_; }

private:
    ScsiResult result_;
};

// MMC command layer on top of a ScsiTransport.
class CdDrive {
public:
    explicit CdDrive(ScsiTransport& transport) : transport_(transport) {}

    DriveInfo inquiry();          // throws ScsiError
    bool isReady();
    Toc readToc();                // throws ScsiError / std::runtime_error

    // Reads `count` CD-DA sectors (count * kSectorBytes bytes) with READ CD.
    // Does not throw: the caller decides how to retry.
    ScsiResult readAudio(uint32_t lba, uint32_t count, uint8_t* out);

private:
    ScsiTransport& transport_;
};

}  // namespace cdr
