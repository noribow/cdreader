#include "bot_transport.h"

#include <algorithm>
#include <cstring>

namespace cdr::usb {

namespace {

constexpr unsigned kDefaultTimeoutMs = 30000;
constexpr unsigned kControlTimeoutMs = 5000;
constexpr uint8_t kCswPassed = 0;
constexpr uint8_t kCswFailed = 1;
constexpr uint8_t kCswPhaseError = 2;
constexpr uint8_t kRequestSenseLength = 18;

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

const char* statusName(UsbStatus status) {
    switch (status) {
        case UsbStatus::Ok: return "ok";
        case UsbStatus::Stall: return "stall";
        case UsbStatus::Timeout: return "timeout";
        case UsbStatus::Error: return "error";
    }
    return "error";
}

std::string describe(const char* phase, const UsbTransfer& t) {
    std::string s = std::string(phase) + " " + statusName(t.status);
    if (!t.error.empty()) s += " (" + t.error + ")";
    return s;
}

}  // namespace

ScsiResult BulkOnlyTransport::fail(std::string error) {
    ScsiResult r;
    r.error = "USB bulk-only transport: " + error;
    return r;
}

ScsiResult BulkOnlyTransport::execute(const uint8_t* cdb, size_t cdbLength, void* data, size_t dataLength,
                                      DataDirection direction, unsigned timeoutSeconds) {
    if (cdb == nullptr || cdbLength < 1 || cdbLength > 16) return fail("invalid CDB length");
    if (direction == DataDirection::None) dataLength = 0;
    if (uint64_t(dataLength) > 0xFFFFFFFFu) return fail("transfer too large");
    const unsigned timeoutMs = timeoutSeconds > 0 ? timeoutSeconds * 1000 : kDefaultTimeoutMs;

    uint8_t cswStatus = 0;
    ScsiResult result = command(cdb, cdbLength, static_cast<uint8_t*>(data), dataLength, direction,
                                timeoutMs, cswStatus);
    if (result.transportOk && cswStatus == kCswFailed) {
        result.status = 0x02;  // CHECK CONDITION
        requestSense(result, timeoutMs);
    }
    return result;
}

// One CBW / data / CSW exchange. On success `cswStatus` is 0 (passed) or 1
// (failed); every other outcome is returned as a transport error after
// reset recovery, leaving the device ready for the next command.
ScsiResult BulkOnlyTransport::command(const uint8_t* cdb, size_t cdbLength, uint8_t* data, size_t dataLength,
                                      DataDirection direction, unsigned timeoutMs, uint8_t& cswStatus) {
    const uint32_t tag = ++tag_;
    uint8_t cbw[kCbwLength] = {};
    put32(cbw, kCbwSignature);
    put32(cbw + 4, tag);
    put32(cbw + 8, uint32_t(dataLength));
    cbw[12] = direction == DataDirection::In ? 0x80 : 0x00;
    cbw[13] = lun_ & 0x0F;
    cbw[14] = uint8_t(cdbLength);
    std::memcpy(cbw + 15, cdb, cdbLength);

    UsbTransfer t = endpoints_.bulkOut(cbw, sizeof cbw, timeoutMs);
    if (t.status != UsbStatus::Ok || t.length != sizeof cbw) {
        resetRecovery();
        return fail(describe("sending CBW:", t));
    }

    // Data phase. A stalled endpoint ends it early: the CSW still follows.
    size_t moved = 0;
    Status status;
    bool haveStatus = false;
    if (dataLength > 0) {
        const bool in = direction == DataDirection::In;
        t = in ? endpoints_.bulkIn(data, dataLength, timeoutMs) : endpoints_.bulkOut(data, dataLength, timeoutMs);
        moved = std::min(t.length, dataLength);
        if (t.status == UsbStatus::Stall) {
            endpoints_.clearHalt(in ? BulkEndpoint::In : BulkEndpoint::Out);
        } else if (t.status != UsbStatus::Ok) {
            resetRecovery();
            return fail(describe("data phase:", t));
        } else if (in && moved == kCswLength && dataLength != kCswLength && get32(data) == kCswSignature &&
                   get32(data + 4) == tag) {
            // Some devices skip the data phase of a failing command and send
            // the CSW right away.
            status.ok = true;
            status.residue = get32(data + 8);
            status.status = data[12];
            haveStatus = true;
            moved = 0;
        }
    }

    if (!haveStatus) status = readStatus(tag, timeoutMs);
    if (!status.ok) {
        resetRecovery();
        return fail(status.error);
    }
    if (status.status == kCswPhaseError) {
        resetRecovery();
        return fail("phase error");
    }
    if (status.status != kCswPassed && status.status != kCswFailed) {
        resetRecovery();
        return fail("invalid CSW status " + std::to_string(status.status));
    }

    ScsiResult result;
    result.transportOk = true;
    cswStatus = status.status;
    // The residue is what the device did not process; never report more than was moved.
    const size_t byResidue = dataLength - std::min<size_t>(status.residue, dataLength);
    result.transferred = std::min(moved, byResidue);
    if (direction == DataDirection::In && dataLength > 0)
        std::memset(data + result.transferred, 0, dataLength - result.transferred);  // no stale bytes
    return result;
}

BulkOnlyTransport::Status BulkOnlyTransport::readStatus(uint32_t tag, unsigned timeoutMs) {
    Status s;
    uint8_t csw[kCswLength] = {};
    UsbTransfer t;
    // BOT 6.7.2: if the CSW read stalls, clear the halt and try once more.
    for (int attempt = 0; attempt < 2; ++attempt) {
        t = endpoints_.bulkIn(csw, sizeof csw, timeoutMs);
        if (t.status == UsbStatus::Stall) {
            endpoints_.clearHalt(BulkEndpoint::In);
            continue;
        }
        if (t.status == UsbStatus::Ok && t.length == 0) continue;  // stray zero-length packet
        break;
    }
    if (t.status != UsbStatus::Ok) {
        s.error = describe("reading CSW:", t);
        return s;
    }
    if (t.length != kCswLength) {
        s.error = "invalid CSW length " + std::to_string(t.length);
        return s;
    }
    if (get32(csw) != kCswSignature) {
        s.error = "invalid CSW signature";
        return s;
    }
    if (get32(csw + 4) != tag) {
        s.error = "CSW tag mismatch";
        return s;
    }
    s.ok = true;
    s.residue = get32(csw + 8);
    s.status = csw[12];
    return s;
}

void BulkOnlyTransport::requestSense(ScsiResult& result, unsigned timeoutMs) {
    const uint8_t cdb[6] = {0x03, 0, 0, 0, kRequestSenseLength, 0};
    uint8_t sense[kRequestSenseLength] = {};
    uint8_t cswStatus = 0;
    ScsiResult r = command(cdb, sizeof cdb, sense, sizeof sense, DataDirection::In, timeoutMs, cswStatus);
    // If even REQUEST SENSE fails, report CHECK CONDITION without sense data.
    if (r.transportOk && cswStatus == kCswPassed) result.sense = parseSense(sense, r.transferred);
}

bool BulkOnlyTransport::resetRecovery() {
    const UsbTransfer t = endpoints_.controlTransfer(0x21, kResetRequest, 0, interface_, nullptr, 0,
                                                     kControlTimeoutMs);
    const bool in = endpoints_.clearHalt(BulkEndpoint::In);
    const bool out = endpoints_.clearHalt(BulkEndpoint::Out);
    return t.status == UsbStatus::Ok && in && out;
}

uint8_t BulkOnlyTransport::getMaxLun() {
    uint8_t lun = 0;
    const UsbTransfer t = endpoints_.controlTransfer(0xA1, kGetMaxLunRequest, 0, interface_, &lun, 1,
                                                     kControlTimeoutMs);
    return t.status == UsbStatus::Ok && t.length == 1 ? uint8_t(lun & 0x0F) : 0;
}

}  // namespace cdr::usb
