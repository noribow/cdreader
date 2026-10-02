#include "fake_usb_device.h"

#include <algorithm>
#include <cstring>
#include <utility>

using cdr::usb::BulkEndpoint;
using cdr::usb::BulkOnlyTransport;
using cdr::usb::UsbStatus;
using cdr::usb::UsbTransfer;

namespace {

uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

UsbTransfer done(UsbStatus status, size_t length = 0) {
    UsbTransfer t;
    t.status = status;
    t.length = length;
    if (status == UsbStatus::Timeout) t.error = "timed out";
    return t;
}

}  // namespace

UsbTransfer FakeUsbDevice::bulkOut(const uint8_t* data, size_t length, unsigned) {
    if (haltedOut_) return done(UsbStatus::Stall);
    if (phase_ == Phase::DataOut) {
        const size_t n = std::min(length, buffer_.size() - position_);
        std::memcpy(buffer_.data() + position_, data, n);
        position_ += n;
        if (position_ == buffer_.size()) {
            const cdr::ScsiResult r = target_.execute(cdb_.data(), cdb_.size(), buffer_.data(), buffer_.size(),
                                                      cdr::DataDirection::Out, 30);
            finish(r, buffer_.size());
        }
        return done(UsbStatus::Ok, n);
    }
    const bool validCbw = phase_ == Phase::Command && length == BulkOnlyTransport::kCbwLength &&
                          get32(data) == BulkOnlyTransport::kCbwSignature && data[14] >= 1 && data[14] <= 16;
    if (!validCbw || injectCbwStall) {
        injectCbwStall = false;
        haltedIn_ = haltedOut_ = true;
        needsReset_ = true;
        return done(UsbStatus::Stall);
    }
    receiveCbw(data);
    return done(UsbStatus::Ok, length);
}

void FakeUsbDevice::receiveCbw(const uint8_t* cbw) {
    ++commands;
    tag_ = get32(cbw + 4);
    lastTag = tag_;
    lastTransferLength = get32(cbw + 8);
    lastFlags = cbw[12];
    lastLun = cbw[13];
    cdb_.assign(cbw + 15, cbw + 15 + cbw[14]);
    opcodes.push_back(cdb_[0]);
    badSignature_ = std::exchange(injectBadSignature, false);
    wrongTag_ = std::exchange(injectWrongTag, false);
    cswStall_ = std::exchange(injectCswStall, false);
    buffer_.clear();
    position_ = 0;

    const uint32_t length = lastTransferLength;
    const bool in = (lastFlags & 0x80) != 0;

    if (std::exchange(injectPhaseError, false)) {
        status_ = 2;
        residue_ = length;
        if (length > 0) {
            if (in) haltedIn_ = true; else haltedOut_ = true;
        }
        phase_ = Phase::Status;
        return;
    }
    if (length > 0 && !in) {
        ++dataOutCommands;
        buffer_.assign(length, 0);
        phase_ = Phase::DataOut;
        return;
    }
    if (cdb_[0] == 0x03) {  // REQUEST SENSE: the device reports its stored sense
        ++requestSenses;
        uint8_t sense[18] = {0x70, 0, sense_.key, 0, 0, 0, 0, 10, 0, 0, 0, 0, sense_.asc, sense_.ascq};
        buffer_.assign(sense, sense + std::min<size_t>(sizeof sense, length));
        sense_ = {};
        status_ = 0;
        residue_ = length - uint32_t(buffer_.size());
        phase_ = Phase::DataIn;
        return;
    }
    buffer_.assign(length, 0);
    const cdr::ScsiResult r = target_.execute(cdb_.data(), cdb_.size(), buffer_.data(), length,
                                              length > 0 ? cdr::DataDirection::In : cdr::DataDirection::None, 30);
    if (length == 0 || !r.ok()) {
        finish(r, 0);
        return;
    }
    // The target may move less than asked (e.g. a drive ignoring the READ CD
    // error field); the rest is residue, as a real bridge reports it.
    const size_t produced = std::min<size_t>(r.transferred, length);
    const size_t send = produced - std::min<size_t>(injectShortBy, produced);
    injectShortBy = 0;
    buffer_.resize(send);
    status_ = 0;
    residue_ = uint32_t(length - send);
    phase_ = Phase::DataIn;
}

// Sets the status for a completed command; a failing data-in command sends no data.
void FakeUsbDevice::finish(const cdr::ScsiResult& r, size_t moved) {
    status_ = r.ok() ? 0 : 1;
    if (!r.ok()) sense_ = r.transportOk ? r.sense : cdr::SenseInfo{0x4, 0x44, 0x00};  // internal target failure
    residue_ = uint32_t(lastTransferLength - moved);
    const bool dataIn = (lastFlags & 0x80) != 0 && lastTransferLength > 0;
    buffer_.clear();
    position_ = 0;
    phase_ = Phase::Status;
    if (dataIn && !r.ok()) {
        if (cswInsteadOfDataOnError) {
            // falls through to the status phase: the host's data read gets the CSW
        } else if (stallDataInOnError) {
            haltedIn_ = true;
        } else {
            phase_ = Phase::DataIn;  // zero-length data phase
        }
    }
}

UsbTransfer FakeUsbDevice::bulkIn(uint8_t* data, size_t length, unsigned) {
    if (haltedIn_) return done(UsbStatus::Stall);
    if (phase_ == Phase::DataIn) {
        if (std::exchange(injectDataTimeout, false)) return done(UsbStatus::Timeout);
        const size_t n = std::min(length, buffer_.size() - position_);
        std::memcpy(data, buffer_.data() + position_, n);
        position_ += n;
        if (position_ == buffer_.size()) phase_ = Phase::Status;
        return done(UsbStatus::Ok, n);
    }
    if (phase_ == Phase::Status) {
        if (std::exchange(cswStall_, false)) {
            haltedIn_ = true;
            return done(UsbStatus::Stall);
        }
        uint8_t csw[BulkOnlyTransport::kCswLength] = {};
        put32(csw, badSignature_ ? 0x12345678 : BulkOnlyTransport::kCswSignature);
        put32(csw + 4, wrongTag_ ? tag_ + 100 : tag_);
        put32(csw + 8, residue_);
        csw[12] = status_;
        const size_t n = std::min(length, sizeof csw);
        std::memcpy(data, csw, n);
        phase_ = Phase::Command;
        return done(UsbStatus::Ok, n);
    }
    return done(UsbStatus::Timeout);  // nothing to send
}

bool FakeUsbDevice::clearHalt(BulkEndpoint endpoint) {
    if (endpoint == BulkEndpoint::In) {
        ++clearHaltsIn;
        if (!needsReset_) haltedIn_ = false;
    } else {
        ++clearHaltsOut;
        if (!needsReset_) haltedOut_ = false;
    }
    return true;
}

UsbTransfer FakeUsbDevice::controlTransfer(uint8_t requestType, uint8_t request, uint16_t, uint16_t index,
                                           uint8_t* data, uint16_t length, unsigned) {
    if (index != interface_) return done(UsbStatus::Stall);
    if (requestType == 0x21 && request == BulkOnlyTransport::kResetRequest && length == 0) {
        ++resets;
        needsReset_ = false;
        phase_ = Phase::Command;
        injectDataTimeout = false;
        return done(UsbStatus::Ok);  // endpoint halts stay until cleared
    }
    if (requestType == 0xA1 && request == BulkOnlyTransport::kGetMaxLunRequest && length == 1) {
        data[0] = 0;
        return done(UsbStatus::Ok, 1);
    }
    return done(UsbStatus::Stall);
}
