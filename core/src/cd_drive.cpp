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

namespace {

// READ CD (BEh), expected sector type CD-DA. Byte 9: 0x10 = user data, plus
// the error field in bits 2..1 (01b = C2 error pointers); byte 10: the
// sub-channel selection. `perSector` is what the drive returns per sector.
ScsiResult readCd(ScsiTransport& transport, uint32_t lba, uint32_t count, uint8_t byte9, uint8_t byte10,
                  size_t perSector, uint8_t* out) {
    const uint8_t cdb[12] = {
        0xBE, 0x04,
        uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba),
        uint8_t(count >> 16), uint8_t(count >> 8), uint8_t(count),
        byte9, byte10, 0x00,
    };
    const size_t bytes = size_t(count) * perSector;
    ScsiResult r = transport.execute(cdb, sizeof cdb, out, bytes, DataDirection::In, kReadTimeout);
    if (r.ok() && r.transferred != bytes) {
        r.transportOk = false;
        r.shortRead = true;
        r.error = "short read (" + std::to_string(r.transferred) + " of " + std::to_string(bytes) + " bytes)";
    }
    return r;
}

}  // namespace

ScsiResult CdDrive::readAudio(uint32_t lba, uint32_t count, uint8_t* out) {
    // User data only, no error field, no sub-channel.
    return readCd(transport_, lba, count, 0x10, 0x00, kSectorBytes, out);
}

ScsiResult CdDrive::readAudioWithC2(uint32_t lba, uint32_t count, uint8_t* out) {
    // Error field 01b (C2 error block data, 294 bytes): the format every C2
    // capable drive implements. 10b adds a block error byte and a pad byte
    // (296 bytes) that we would not use, and fewer drives accept it.
    return readCd(transport_, lba, count, 0x10 | 0x02, 0x00, kSectorWithC2Bytes, out);
}

ScsiResult CdDrive::readAudioWithSubChannel(uint32_t lba, uint32_t count, SubChannelSelection selection,
                                            uint8_t* out) {
    return readCd(transport_, lba, count, 0x10, uint8_t(selection),
                  kSectorBytes + subChannelBytesPerSector(selection), out);
}

ScsiResult CdDrive::forceUnitAccess(uint32_t lba) {
    // READ(12): byte 1 bit 3 = FUA, bytes 2..5 the LBA, bytes 6..9 the
    // transfer length (0: no blocks, no data phase).
    const uint8_t cdb[12] = {
        0xA8, 0x08,
        uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba),
        0, 0, 0, 0,
        0, 0,
    };
    return transport_.execute(cdb, sizeof cdb, nullptr, 0, DataDirection::None, kReadTimeout);
}

DriveCapabilities DriveCapabilities::parse(const uint8_t* data, size_t length) {
    DriveCapabilities caps;
    if (length < 8) {
        caps.error = "MODE SENSE response too short";
        return caps;
    }
    // Mode parameter header (10): mode data length (excluding itself), ...,
    // block descriptor length in bytes 6..7.
    const size_t end = std::min(length, (size_t(data[0]) << 8 | data[1]) + 2);
    const size_t page = 8 + (size_t(data[6]) << 8 | data[7]);
    if (page + 6 > end) {
        caps.error = "MODE SENSE response without capabilities page";
        return caps;
    }
    if ((data[page] & 0x3F) != 0x2A || data[page + 1] < 4) {
        caps.error = "MODE SENSE returned another page";
        return caps;
    }
    caps.valid = true;
    caps.c2Pointers = (data[page + 5] & 0x10) != 0;
    // Bytes 12..13: buffer size in KB, when the page (length byte + 2) and
    // the response reach that far.
    if (data[page + 1] >= 12 && page + 14 <= end) caps.bufferKB = uint32_t(data[page + 12]) << 8 | data[page + 13];
    return caps;
}

DriveCapabilities CdDrive::readCapabilities() {
    // MODE SENSE(10), DBD = 1 (no block descriptors), current values of page 2Ah.
    std::vector<uint8_t> data(256);
    const uint8_t cdb[10] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, uint8_t(data.size() >> 8), uint8_t(data.size()), 0};
    const ScsiResult r =
        transport_.execute(cdb, sizeof cdb, data.data(), data.size(), DataDirection::In, kCommandTimeout);
    if (!r.ok()) {
        DriveCapabilities caps;
        caps.error = "MODE SENSE failed: " + r.describe();
        return caps;
    }
    return DriveCapabilities::parse(data.data(), std::min(r.transferred, data.size()));
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
