#include "usbdevfs_endpoints.h"

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace cdr::usb {

namespace {

// Older kernels reject usbdevfs bulk transfers above 16 KiB (libusb splits
// them the same way). A multiple of every bulk max packet size, so a short
// chunk always means the device ended the transfer.
constexpr size_t kMaxChunk = 16384;

int ioctlRetry(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

UsbTransfer fromErrno(int err, size_t moved) {
    UsbTransfer t;
    t.length = moved;
    switch (err) {
        case EPIPE: t.status = UsbStatus::Stall; break;
        case ETIMEDOUT: t.status = UsbStatus::Timeout; t.error = "timed out"; break;
        case ENODEV:
        case ESHUTDOWN: t.status = UsbStatus::Error; t.error = "device disconnected"; break;
        default: t.status = UsbStatus::Error; t.error = std::strerror(err); break;
    }
    return t;
}

}  // namespace

UsbDevfsEndpoints::~UsbDevfsEndpoints() {
    if (claimed_) {
        unsigned int ifno = unsigned(interface_);
        ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &ifno);
    }
}

bool UsbDevfsEndpoints::claimInterface(std::string& error) {
    unsigned int ifno = unsigned(interface_);
    if (ioctlRetry(fd_, USBDEVFS_CLAIMINTERFACE, &ifno) == 0) {
        claimed_ = true;
        return true;
    }
    if (errno == EBUSY) {
        // A kernel driver owns the interface: detach it and try again.
        usbdevfs_ioctl command = {};
        command.ifno = interface_;
        command.ioctl_code = USBDEVFS_DISCONNECT;
        command.data = nullptr;
        ioctlRetry(fd_, USBDEVFS_IOCTL, &command);
        if (ioctlRetry(fd_, USBDEVFS_CLAIMINTERFACE, &ifno) == 0) {
            claimed_ = true;
            return true;
        }
    }
    error = std::string("cannot claim USB interface: ") + std::strerror(errno);
    return false;
}

UsbTransfer UsbDevfsEndpoints::bulk(uint8_t endpoint, uint8_t* data, size_t length, unsigned timeoutMs) {
    size_t moved = 0;
    while (true) {
        const size_t chunk = std::min(kMaxChunk, length - moved);
        usbdevfs_bulktransfer xfer = {};
        xfer.ep = endpoint;
        xfer.len = unsigned(chunk);
        xfer.timeout = timeoutMs;
        xfer.data = data + moved;
        const int r = ioctlRetry(fd_, USBDEVFS_BULK, &xfer);
        if (r < 0) return fromErrno(errno, moved);
        moved += size_t(r);
        if (size_t(r) < chunk || moved >= length) break;  // short packet or done
    }
    UsbTransfer t;
    t.status = UsbStatus::Ok;
    t.length = moved;
    return t;
}

UsbTransfer UsbDevfsEndpoints::bulkOut(const uint8_t* data, size_t length, unsigned timeoutMs) {
    // usbdevfs takes a non-const buffer but only reads it for OUT endpoints.
    return bulk(out_, const_cast<uint8_t*>(data), length, timeoutMs);
}

UsbTransfer UsbDevfsEndpoints::bulkIn(uint8_t* data, size_t length, unsigned timeoutMs) {
    return bulk(in_, data, length, timeoutMs);
}

bool UsbDevfsEndpoints::clearHalt(BulkEndpoint endpoint) {
    unsigned int ep = endpoint == BulkEndpoint::In ? in_ : out_;
    return ioctlRetry(fd_, USBDEVFS_CLEAR_HALT, &ep) == 0;
}

UsbTransfer UsbDevfsEndpoints::controlTransfer(uint8_t requestType, uint8_t request, uint16_t value,
                                               uint16_t index, uint8_t* data, uint16_t length,
                                               unsigned timeoutMs) {
    usbdevfs_ctrltransfer xfer = {};
    xfer.bRequestType = requestType;
    xfer.bRequest = request;
    xfer.wValue = value;
    xfer.wIndex = index;
    xfer.wLength = length;
    xfer.timeout = timeoutMs;
    xfer.data = data;
    const int r = ioctlRetry(fd_, USBDEVFS_CONTROL, &xfer);
    if (r < 0) return fromErrno(errno, 0);
    UsbTransfer t;
    t.status = UsbStatus::Ok;
    t.length = size_t(r);
    return t;
}

}  // namespace cdr::usb
