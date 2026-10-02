#pragma once

#include <cstdint>
#include <vector>

#include "bot_transport.h"
#include "cdreader/scsi.h"

// In-memory USB mass storage device (Bulk-Only Transport) in front of a
// ScsiTransport such as FakeDrive: it parses CBWs, runs the SCSI command on
// the target, answers REQUEST SENSE itself and sends CSWs. Endpoint halts
// and the reset rules of the BOT specification are modelled so that error
// recovery can be tested; the inject* flags apply to the next command only.
class FakeUsbDevice : public cdr::usb::UsbBulkEndpoints {
public:
    explicit FakeUsbDevice(cdr::ScsiTransport& target, uint16_t interfaceNumber = 0)
        : target_(target), interface_(interfaceNumber) {}

    // A failing data-in command stalls bulk-in (true) or sends no data (false).
    bool stallDataInOnError = true;
    // A failing data-in command sends the CSW in place of the data.
    bool cswInsteadOfDataOnError = false;

    bool injectCbwStall = false;     // bulk-out stalls on the CBW
    bool injectCswStall = false;     // the first CSW read stalls
    bool injectBadSignature = false;
    bool injectWrongTag = false;
    bool injectPhaseError = false;   // CSW status 2, no data
    bool injectDataTimeout = false;  // the data phase never completes
    size_t injectShortBy = 0;        // send this many bytes less (reported as residue)

    int commands = 0;      // CBWs accepted
    int dataOutCommands = 0;
    int requestSenses = 0;
    int resets = 0;        // Bulk-Only Mass Storage Resets
    int clearHaltsIn = 0;
    int clearHaltsOut = 0;
    std::vector<uint8_t> opcodes;  // first CDB byte of every accepted CBW
    uint32_t lastTag = 0;
    uint8_t lastLun = 0;
    uint8_t lastFlags = 0;
    uint32_t lastTransferLength = 0;

    bool haltedIn() const { return haltedIn_; }
    bool haltedOut() const { return haltedOut_; }

    cdr::usb::UsbTransfer bulkOut(const uint8_t* data, size_t length, unsigned timeoutMs) override;
    cdr::usb::UsbTransfer bulkIn(uint8_t* data, size_t length, unsigned timeoutMs) override;
    bool clearHalt(cdr::usb::BulkEndpoint endpoint) override;
    cdr::usb::UsbTransfer controlTransfer(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                                          uint8_t* data, uint16_t length, unsigned timeoutMs) override;

private:
    enum class Phase { Command, DataIn, DataOut, Status };

    void receiveCbw(const uint8_t* cbw);
    void finish(const cdr::ScsiResult& r, size_t moved);

    cdr::ScsiTransport& target_;
    uint16_t interface_;
    Phase phase_ = Phase::Command;
    bool haltedIn_ = false;
    bool haltedOut_ = false;
    bool needsReset_ = false;  // after an invalid CBW only a reset helps (BOT 6.6.1)

    uint32_t tag_ = 0;
    std::vector<uint8_t> cdb_;
    std::vector<uint8_t> buffer_;  // data to send or being received
    size_t position_ = 0;
    uint32_t residue_ = 0;
    uint8_t status_ = 0;
    cdr::SenseInfo sense_;
    bool badSignature_ = false;
    bool wrongTag_ = false;
    bool cswStall_ = false;
};
