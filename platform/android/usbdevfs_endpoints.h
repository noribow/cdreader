#pragma once

#include <cstdint>
#include <string>

#include "bot_transport.h"

namespace cdr::usb {

// UsbBulkEndpoints over Linux usbdevfs ioctls. On Android `fd` comes from
// UsbDeviceConnection.getFileDescriptor() (the app has no permission to open
// /dev/bus/usb itself); this is how libusb works on Android as well. The
// file descriptor stays owned by the caller.
class UsbDevfsEndpoints : public UsbBulkEndpoints {
public:
    // Endpoint addresses include the direction bit (e.g. 0x81 / 0x02).
    UsbDevfsEndpoints(int fd, int interfaceNumber, uint8_t endpointIn, uint8_t endpointOut)
        : fd_(fd), interface_(interfaceNumber), in_(endpointIn), out_(endpointOut) {}
    ~UsbDevfsEndpoints() override;
    UsbDevfsEndpoints(const UsbDevfsEndpoints&) = delete;
    UsbDevfsEndpoints& operator=(const UsbDevfsEndpoints&) = delete;

    // Claims the interface, detaching a kernel driver (usb-storage) if one is
    // bound. Succeeds as well when the Java side already claimed it.
    bool claimInterface(std::string& error);

    UsbTransfer bulkOut(const uint8_t* data, size_t length, unsigned timeoutMs) override;
    UsbTransfer bulkIn(uint8_t* data, size_t length, unsigned timeoutMs) override;
    bool clearHalt(BulkEndpoint endpoint) override;
    UsbTransfer controlTransfer(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                                uint8_t* data, uint16_t length, unsigned timeoutMs) override;

private:
    UsbTransfer bulk(uint8_t endpoint, uint8_t* data, size_t length, unsigned timeoutMs);

    int fd_;
    int interface_;
    uint8_t in_;
    uint8_t out_;
    bool claimed_ = false;
};

}  // namespace cdr::usb
