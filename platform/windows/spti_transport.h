#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cdreader/scsi.h"

namespace cdr::win {

// Drive letters (e.g. 'D') of all optical drives on the system.
std::vector<char> listOpticalDrives();

// SCSI pass-through (IOCTL_SCSI_PASS_THROUGH_DIRECT) to an optical drive.
class SptiTransport : public ScsiTransport {
public:
    // Opens \\.\X: for the given drive letter. Throws std::runtime_error.
    static std::unique_ptr<SptiTransport> open(char driveLetter);
    ~SptiTransport() override;

    ScsiResult execute(const uint8_t* cdb, size_t cdbLength,
                       void* data, size_t dataLength,
                       DataDirection direction,
                       unsigned timeoutSeconds) override;

private:
    SptiTransport(void* handle, void* buffer, size_t bufferSize)
        : handle_(handle), buffer_(buffer), bufferSize_(bufferSize) {}

    void* handle_;      // HANDLE
    void* buffer_;      // page-aligned bounce buffer (satisfies any adapter alignment)
    size_t bufferSize_;
};

// Human readable text for a Win32 error code.
std::string win32ErrorMessage(unsigned long code);

}  // namespace cdr::win
