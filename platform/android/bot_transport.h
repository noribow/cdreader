#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cdreader/scsi.h"

namespace cdr::usb {

enum class UsbStatus {
    Ok,       // transfer completed (possibly short)
    Stall,    // the endpoint answered STALL (halted)
    Timeout,
    Error,    // anything else, e.g. the device was unplugged
};

struct UsbTransfer {
    UsbStatus status = UsbStatus::Error;
    size_t length = 0;  // bytes moved before the transfer ended
    std::string error;  // description for Error / Timeout
};

enum class BulkEndpoint { In, Out };

// Raw endpoint I/O of a USB mass storage interface. Android implements it
// with usbdevfs ioctls on the file descriptor of a UsbDeviceConnection; the
// unit tests use an in-memory device.
class UsbBulkEndpoints {
public:
    virtual ~UsbBulkEndpoints() = default;

    virtual UsbTransfer bulkOut(const uint8_t* data, size_t length, unsigned timeoutMs) = 0;
    // Reads up to `length` bytes; a short packet from the device ends the transfer early.
    virtual UsbTransfer bulkIn(uint8_t* data, size_t length, unsigned timeoutMs) = 0;
    // CLEAR_FEATURE(ENDPOINT_HALT), also resetting the host side data toggle.
    virtual bool clearHalt(BulkEndpoint endpoint) = 0;
    // Control transfer on endpoint 0. `data` may be null when `length` is 0.
    virtual UsbTransfer controlTransfer(uint8_t requestType, uint8_t request, uint16_t value,
                                        uint16_t index, uint8_t* data, uint16_t length,
                                        unsigned timeoutMs) = 0;
};

// USB Mass Storage Class Bulk-Only Transport (BOT, "BBB") 1.0: wraps each
// SCSI command in a Command Block Wrapper, moves the data and reads the
// Command Status Wrapper. A failed command (CSW status 1) is followed by
// REQUEST SENSE so that ScsiResult looks like the one from SPTI. Phase
// errors, invalid CSWs and broken transfers trigger the reset recovery of
// BOT 5.3.4 (Bulk-Only Mass Storage Reset + clearing both endpoint halts).
class BulkOnlyTransport : public ScsiTransport {
public:
    static constexpr uint32_t kCbwSignature = 0x43425355;  // "USBC"
    static constexpr uint32_t kCswSignature = 0x53425355;  // "USBS"
    static constexpr size_t kCbwLength = 31;
    static constexpr size_t kCswLength = 13;
    static constexpr uint8_t kResetRequest = 0xFF;        // Bulk-Only Mass Storage Reset
    static constexpr uint8_t kGetMaxLunRequest = 0xFE;

    // `interfaceNumber` addresses class requests (reset); `lun` selects the logical unit.
    BulkOnlyTransport(UsbBulkEndpoints& endpoints, uint8_t interfaceNumber, uint8_t lun = 0)
        : endpoints_(endpoints), interface_(interfaceNumber), lun_(lun) {}

    ScsiResult execute(const uint8_t* cdb, size_t cdbLength,
                       void* data, size_t dataLength,
                       DataDirection direction,
                       unsigned timeoutSeconds) override;

    // Bulk-Only Mass Storage Reset followed by clearing both endpoint halts.
    bool resetRecovery();

    // GET MAX LUN; returns 0 when the device does not support it (it may stall).
    uint8_t getMaxLun();

    uint32_t lastTag() const { return tag_; }

private:
    struct Status {
        bool ok = false;
        uint8_t status = 0;
        uint32_t residue = 0;
        std::string error;
    };

    ScsiResult command(const uint8_t* cdb, size_t cdbLength, uint8_t* data, size_t dataLength,
                       DataDirection direction, unsigned timeoutMs, uint8_t& cswStatus);
    Status readStatus(uint32_t tag, unsigned timeoutMs);
    void requestSense(ScsiResult& result, unsigned timeoutMs);
    ScsiResult fail(std::string error);

    UsbBulkEndpoints& endpoints_;
    uint8_t interface_;
    uint8_t lun_;
    uint32_t tag_ = 0;
};

}  // namespace cdr::usb
