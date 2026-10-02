#include "spti_transport.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace cdr::win {

namespace {

constexpr size_t kBounceBufferSize = 64 * 1024;

struct SptdWithSense {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG filler;  // keeps the sense buffer aligned
    UCHAR sense[32];
};

}  // namespace

std::string win32ErrorMessage(unsigned long code) {
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::string message;
    if (n > 0 && text != nullptr) {
        int len = WideCharToMultiByte(CP_UTF8, 0, text, int(n), nullptr, 0, nullptr, nullptr);
        message.resize(size_t(len));
        WideCharToMultiByte(CP_UTF8, 0, text, int(n), message.data(), len, nullptr, nullptr);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            message.pop_back();
    }
    if (text != nullptr) LocalFree(text);
    return message + " (error " + std::to_string(code) + ")";
}

std::vector<char> listOpticalDrives() {
    std::vector<char> drives;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) == DRIVE_CDROM) drives.push_back(char('A' + i));
    }
    return drives;
}

std::unique_ptr<SptiTransport> SptiTransport::open(char driveLetter) {
    const wchar_t path[] = {L'\\', L'\\', L'.', L'\\', wchar_t(driveLetter), L':', L'\0'};
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(std::string("cannot open drive ") + driveLetter + ": " +
                                 win32ErrorMessage(GetLastError()));
    }
    void* buffer = VirtualAlloc(nullptr, kBounceBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buffer == nullptr) {
        CloseHandle(h);
        throw std::runtime_error("out of memory");
    }
    return std::unique_ptr<SptiTransport>(new SptiTransport(h, buffer, kBounceBufferSize));
}

SptiTransport::~SptiTransport() {
    VirtualFree(buffer_, 0, MEM_RELEASE);
    CloseHandle(static_cast<HANDLE>(handle_));
}

ScsiResult SptiTransport::execute(const uint8_t* cdb, size_t cdbLength, void* data, size_t dataLength,
                                  DataDirection direction, unsigned timeoutSeconds) {
    ScsiResult result;
    if (cdbLength > 16 || dataLength > bufferSize_) {
        result.error = "request too large for SCSI pass-through";
        return result;
    }

    SptdWithSense req;
    std::memset(&req, 0, sizeof req);
    req.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    req.sptd.CdbLength = UCHAR(cdbLength);
    req.sptd.SenseInfoLength = sizeof req.sense;
    req.sptd.SenseInfoOffset = offsetof(SptdWithSense, sense);
    req.sptd.DataTransferLength = ULONG(dataLength);
    req.sptd.TimeOutValue = timeoutSeconds;
    req.sptd.DataBuffer = dataLength > 0 ? buffer_ : nullptr;
    switch (direction) {
        case DataDirection::In: req.sptd.DataIn = SCSI_IOCTL_DATA_IN; break;
        case DataDirection::Out: req.sptd.DataIn = SCSI_IOCTL_DATA_OUT; break;
        case DataDirection::None: req.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED; break;
    }
    std::memcpy(req.sptd.Cdb, cdb, cdbLength);
    if (direction == DataDirection::Out && dataLength > 0) std::memcpy(buffer_, data, dataLength);

    DWORD returned = 0;
    if (!DeviceIoControl(static_cast<HANDLE>(handle_), IOCTL_SCSI_PASS_THROUGH_DIRECT, &req, sizeof req,
                         &req, sizeof req, &returned, nullptr)) {
        const DWORD err = GetLastError();
        result.error = "SCSI pass-through failed: " + win32ErrorMessage(err);
        if (err == ERROR_ACCESS_DENIED) result.error += " - try running as administrator";
        return result;
    }

    result.transportOk = true;
    result.status = req.sptd.ScsiStatus;
    if (result.status != 0) {
        result.sense = parseSense(req.sense, sizeof req.sense);
    } else if (direction == DataDirection::In && dataLength > 0) {
        // The device may transfer less than requested; never expose stale bytes.
        const size_t got = req.sptd.DataTransferLength < dataLength ? req.sptd.DataTransferLength : dataLength;
        std::memcpy(data, buffer_, got);
        result.transferred = got;
        std::memset(static_cast<uint8_t*>(data) + got, 0, dataLength - got);
    } else if (direction == DataDirection::Out) {
        result.transferred = req.sptd.DataTransferLength;
    }
    return result;
}

}  // namespace cdr::win
