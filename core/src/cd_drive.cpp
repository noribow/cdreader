#include "cdreader/cd_drive.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace cdr {

namespace {

constexpr unsigned kCommandTimeout = 30;
constexpr unsigned kReadTimeout = 60;  // spin-up and retries inside the drive can be slow

std::string trimmed(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

}  // namespace

std::string DriveInfo::displayName() const {
    std::string name = vendor;
    if (!product.empty()) name += (name.empty() ? "" : " ") + product;
    if (!revision.empty()) name += " (" + revision + ")";
    return name;
}

DriveInfo CdDrive::inquiry() {
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};  // INQUIRY
    uint8_t data[36] = {};
    ScsiResult r = transport_.execute(cdb, sizeof cdb, data, sizeof data, DataDirection::In, kCommandTimeout);
    if (!r.ok()) throw ScsiError("INQUIRY failed", r);
    return DriveInfo{trimmed(data + 8, 8), trimmed(data + 16, 16), trimmed(data + 32, 4)};
}

bool CdDrive::isReady() {
    uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};  // TEST UNIT READY
    return transport_.execute(cdb, sizeof cdb, nullptr, 0, DataDirection::None, kCommandTimeout).ok();
}

Toc CdDrive::readToc() {
    // READ TOC/PMA/ATIP, format 0 (TOC), LBA addressing, from track 1.
    // 4-byte header + up to 99 tracks + lead-out, 8 bytes each = 804 bytes.
    std::vector<uint8_t> data(804);
    uint8_t cdb[10] = {0x43, 0x00, 0x00, 0, 0, 0, 1, uint8_t(data.size() >> 8), uint8_t(data.size() & 0xFF), 0};
    ScsiResult r = transport_.execute(cdb, sizeof cdb, data.data(), data.size(), DataDirection::In, kCommandTimeout);
    if (!r.ok()) throw ScsiError("READ TOC failed", r);
    return Toc::parse(data.data(), data.size());
}

ScsiResult CdDrive::readAudio(uint32_t lba, uint32_t count, uint8_t* out) {
    // READ CD: expected sector type CD-DA, user data only, no sub-channel.
    uint8_t cdb[12] = {
        0xBE, 0x04,
        uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba),
        uint8_t(count >> 16), uint8_t(count >> 8), uint8_t(count),
        0x10, 0x00, 0x00,
    };
    const size_t bytes = size_t(count) * kSectorBytes;
    ScsiResult r = transport_.execute(cdb, sizeof cdb, out, bytes, DataDirection::In, kReadTimeout);
    if (r.ok() && r.transferred != bytes) {
        r.transportOk = false;
        r.error = "short read (" + std::to_string(r.transferred) + " of " + std::to_string(bytes) + " bytes)";
    }
    return r;
}

ScsiResult CdDrive::readAudioWithSubChannel(uint32_t lba, uint32_t count, SubChannelSelection selection,
                                            uint8_t* out) {
    // READ CD: expected sector type CD-DA, user data, sub-channel selection.
    uint8_t cdb[12] = {
        0xBE, 0x04,
        uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba),
        uint8_t(count >> 16), uint8_t(count >> 8), uint8_t(count),
        0x10, uint8_t(selection), 0x00,
    };
    const size_t bytes = size_t(count) * (kSectorBytes + subChannelBytesPerSector(selection));
    ScsiResult r = transport_.execute(cdb, sizeof cdb, out, bytes, DataDirection::In, kReadTimeout);
    if (r.ok() && r.transferred != bytes) {
        r.transportOk = false;
        r.error = "short read (" + std::to_string(r.transferred) + " of " + std::to_string(bytes) + " bytes)";
    }
    return r;
}

ScsiResult CdDrive::readSubChannel(SubChannelFormat format, int track, uint8_t* out, size_t length, bool msf) {
    if (length > 0xFFFF) length = 0xFFFF;
    const uint8_t cdb[10] = {
        0x42,
        uint8_t(msf ? 0x02 : 0x00),
        0x40,  // SubQ: return Q sub-channel data
        uint8_t(format),
        0, 0,
        uint8_t(format == SubChannelFormat::Isrc ? track : 0),
        uint8_t(length >> 8), uint8_t(length & 0xFF),
        0,
    };
    return transport_.execute(cdb, sizeof cdb, out, length, DataDirection::In, kCommandTimeout);
}

SubChannelCode CdDrive::readMcn() {
    uint8_t data[kSubChannelResponseBytes] = {};
    const ScsiResult r = readSubChannel(SubChannelFormat::MediaCatalogNumber, 0, data, sizeof data);
    if (!r.ok()) return subChannelError(r);
    return parseMcnResponse(data, std::min(r.transferred, sizeof data));
}

SubChannelCode CdDrive::readIsrc(int track) {
    if (track < 1 || track > 99) {
        SubChannelCode c;
        c.status = SubChannelCode::Status::Invalid;
        c.detail = "track number " + std::to_string(track);
        return c;
    }
    uint8_t data[kSubChannelResponseBytes] = {};
    const ScsiResult r = readSubChannel(SubChannelFormat::Isrc, track, data, sizeof data);
    if (!r.ok()) return subChannelError(r);
    return parseIsrcResponse(data, std::min(r.transferred, sizeof data), track);
}

}  // namespace cdr
