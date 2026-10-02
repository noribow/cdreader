#include "cdreader/subchannel.h"

#include <cstdio>
#include <exception>

#include "cdreader/cd_drive.h"

namespace cdr {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }

SubChannelCode make(SubChannelCode::Status status, std::string value = {}, std::string detail = {}) {
    SubChannelCode c;
    c.status = status;
    c.value = std::move(value);
    c.detail = std::move(detail);
    return c;
}

// Text in bytes [from, from + n) up to the first NUL, for messages: non
// printable bytes become '?'.
std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i] != 0; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '?';
    return s;
}

// Common checks of a format 02h / 03h response; returns false (and sets
// `error`) when it is unusable.
bool checkHeader(const uint8_t* d, size_t length, SubChannelFormat format, SubChannelCode& error) {
    if (d == nullptr || length < kSubChannelResponseBytes) {
        error = make(SubChannelCode::Status::Invalid, {},
                     "short response (" + std::to_string(length) + " of " + std::to_string(kSubChannelResponseBytes) +
                         " bytes)");
        return false;
    }
    // Sub-channel data length: the bytes after the 4-byte header (20).
    const size_t dataLength = size_t(d[2]) << 8 | d[3];
    if (dataLength < kSubChannelResponseBytes - 4) {
        error = make(SubChannelCode::Status::Invalid, {}, "sub-channel data length " + std::to_string(dataLength));
        return false;
    }
    if (d[4] != uint8_t(format)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "format code %02Xh instead of %02Xh", d[4], unsigned(format));
        error = make(SubChannelCode::Status::Invalid, {}, buf);
        return false;
    }
    return true;
}

}  // namespace

bool isValidMcn(const std::string& mcn) {
    if (mcn.size() != 13) return false;
    bool nonZero = false;
    for (char c : mcn) {
        if (!isDigit(c)) return false;
        nonZero |= c != '0';
    }
    return nonZero;
}

bool isValidIsrc(const std::string& isrc) {
    if (isrc.size() != 12) return false;
    for (size_t i = 0; i < 12; ++i) {
        const char c = isrc[i];
        const bool ok = i < 2 ? isUpper(c) : i < 5 ? (isUpper(c) || isDigit(c)) : isDigit(c);
        if (!ok) return false;
    }
    return true;
}

std::string SubChannelCode::describe() const {
    switch (status) {
        case Status::Found: return value;
        case Status::NotPresent: return "not present";
        case Status::Invalid: return "invalid response (" + detail + ")";
        case Status::Unsupported: return "not supported by the drive";
        case Status::Failed: return "read failed (" + detail + ")";
        case Status::Skipped: return detail.empty() ? "not read" : "not read (" + detail + ")";
    }
    return {};
}

// MMC Media Catalog Number data format: byte 8 bit 7 MCVal, bytes 9..21 the
// 13 ASCII digits, then a NUL (and AFRAME on some drives).
SubChannelCode parseMcnResponse(const uint8_t* d, size_t length) {
    SubChannelCode error;
    if (!checkHeader(d, length, SubChannelFormat::MediaCatalogNumber, error)) return error;
    if (!(d[8] & 0x80)) return make(SubChannelCode::Status::NotPresent);
    const std::string mcn(reinterpret_cast<const char*>(d + 9), 13);
    if (mcn == std::string(13, '0')) return make(SubChannelCode::Status::NotPresent);  // MCVal set, no number
    if (!isValidMcn(mcn)) return make(SubChannelCode::Status::Invalid, {}, "MCN \"" + printable(d + 9, 13) + "\"");
    return make(SubChannelCode::Status::Found, mcn);
}

// MMC ISRC data format: byte 5 ADR / control, byte 6 track number, byte 8
// bit 7 TCVal, bytes 9..20 the 12 ISRC characters.
SubChannelCode parseIsrcResponse(const uint8_t* d, size_t length, int track) {
    SubChannelCode error;
    if (!checkHeader(d, length, SubChannelFormat::Isrc, error)) return error;
    // Drives echo the requested track; 0 is accepted (some leave it unset).
    if (d[6] != 0 && d[6] != track)
        return make(SubChannelCode::Status::Invalid, {}, "response for track " + std::to_string(d[6]));
    if (!(d[8] & 0x80)) return make(SubChannelCode::Status::NotPresent);
    const std::string isrc(reinterpret_cast<const char*>(d + 9), 12);
    if (isrc == std::string(12, '0') || isrc == std::string(12, '\0')) return make(SubChannelCode::Status::NotPresent);
    if (!isValidIsrc(isrc)) return make(SubChannelCode::Status::Invalid, {}, "ISRC \"" + printable(d + 9, 12) + "\"");
    return make(SubChannelCode::Status::Found, isrc);
}

SubChannelCode subChannelError(const ScsiResult& r) {
    if (r.transportOk && r.status == 0x02 && r.sense.key == 0x5) return make(SubChannelCode::Status::Unsupported);
    return make(SubChannelCode::Status::Failed, {}, r.describe());
}

// --- Per-sector Q frames --------------------------------------------------------

size_t subChannelBytesPerSector(SubChannelSelection selection) {
    switch (selection) {
        case SubChannelSelection::None: return 0;
        case SubChannelSelection::RawPW: return 96;
        case SubChannelSelection::FormattedQ: return 16;
    }
    return 0;
}

uint16_t subQCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = uint16_t((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

void deinterleaveQ(const uint8_t* rawPw, uint8_t q[12]) {
    for (size_t i = 0; i < 12; ++i) {
        uint8_t v = 0;
        for (size_t b = 0; b < 8; ++b) v = uint8_t(v << 1 | ((rawPw[i * 8 + b] >> 6) & 1));
        q[i] = v;
    }
}

void interleaveQ(const uint8_t q[12], bool pause, uint8_t* rawPw) {
    for (size_t j = 0; j < 96; ++j) {
        const uint8_t bit = (q[j / 8] >> (7 - j % 8)) & 1;
        rawPw[j] = uint8_t((rawPw[j] & 0x3F) | (pause ? 0x80 : 0) | (bit << 6));
    }
}

namespace {

// A BCD byte (00..99), or -1.
int bcd(uint8_t v) {
    const int hi = v >> 4, lo = v & 0x0F;
    return hi > 9 || lo > 9 ? -1 : hi * 10 + lo;
}

// Sectors of an MSF time, or -1 when it is not valid BCD / out of range.
int32_t msfSectors(const uint8_t* msf) {
    const int m = bcd(msf[0]), s = bcd(msf[1]), f = bcd(msf[2]);
    if (m < 0 || s < 0 || s > 59 || f < 0 || f >= int(kSectorsPerSecond)) return -1;
    return (m * 60 + s) * int32_t(kSectorsPerSecond) + f;
}

int32_t be32s(const uint8_t* p) {
    return int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
}

}  // namespace

QFrame parseQ(const uint8_t* q, bool withCrc) {
    QFrame f;
    f.control = q[0] >> 4;
    f.adr = q[0] & 0x0F;
    if (withCrc)
        f.crc = uint16_t(subQCrc16(q, 10) ^ 0xFFFF) == uint16_t(q[10] << 8 | q[11]) ? QFrame::Crc::Valid
                                                                                     : QFrame::Crc::Invalid;
    if (f.adr != 1) {  // MCN / ISRC / unknown mode: no position to decode
        f.parsed = f.adr == 2 || f.adr == 3;
        return f;
    }
    f.track = q[1] == 0xAA ? 170 : bcd(q[1]);
    f.index = bcd(q[2]);
    const int32_t relative = msfSectors(q + 3);
    const int32_t absolute = msfSectors(q + 7);
    f.parsed = f.track >= 1 && (f.track <= 99 || f.track == 170) && f.index >= 0 && relative >= 0 && absolute >= 0;
    if (!f.parsed) return f;
    // The relative time counts down to INDEX 01 in the pregap.
    f.relative = f.index == 0 && f.track != 170 ? -relative : relative;
    f.absoluteLba = absolute - int32_t(kPregapSectors);
    return f;
}

QFrame parseFormattedQ(const uint8_t* data) { return parseQ(data, data[10] != 0 || data[11] != 0); }

QFrame parseRawPwQ(const uint8_t* rawPw) {
    uint8_t q[12];
    deinterleaveQ(rawPw, q);
    return parseQ(q, true);
}

// MMC CD current position data format: byte 5 ADR (high nibble) / CONTROL,
// byte 6 track, byte 7 index (binary), bytes 8..11 absolute and 12..15
// track relative address (LBA, signed).
QFrame parseCurrentPosition(const uint8_t* d, size_t length) {
    QFrame f;
    if (d == nullptr || length < 16 || d[4] != uint8_t(SubChannelFormat::CurrentPosition)) return f;
    f.adr = d[5] >> 4;
    f.control = d[5] & 0x0F;
    f.track = d[6];
    f.index = d[7];
    f.absoluteLba = be32s(d + 8);
    f.relative = be32s(d + 12);
    f.parsed = f.adr == 1 && f.track >= 1 && (f.track <= 99 || f.track == 170) && f.index <= 99;
    return f;
}

std::string DiscCodes::isrc(int track) const {
    const auto it = isrcs.find(track);
    return it != isrcs.end() && it->second.found() ? it->second.value : std::string();
}

std::vector<std::string> DiscCodes::logLines() const {
    std::vector<std::string> lines;
    if (!read) {
        lines.push_back("MCN / ISRC: not read (disabled)");
        return lines;
    }
    lines.push_back("MCN: " + mcn.describe());
    for (const auto& [track, code] : isrcs) {
        char head[32];
        std::snprintf(head, sizeof head, "Track %2d  ISRC: ", track);
        lines.push_back(head + code.describe());
    }
    return lines;
}

void DiscCodes::applyTo(AlbumMetadata& album) const {
    album.mcn = mcnValue();
    album.trackIsrcs.clear();
    for (const auto& [track, code] : isrcs) {
        if (!code.found() || track < 1) continue;
        if (album.trackIsrcs.size() < size_t(track)) album.trackIsrcs.resize(size_t(track));
        album.trackIsrcs[size_t(track - 1)] = code.value;
    }
}

DiscCodes readDiscCodes(CdDrive& drive, const std::vector<Track>& tracks) {
    DiscCodes codes;
    codes.read = true;
    bool unsupported = false;
    auto guarded = [&](auto read) {
        if (unsupported) return make(SubChannelCode::Status::Skipped, {}, "command not supported");
        SubChannelCode c;
        try {
            c = read();
        } catch (const std::exception& e) {  // CdDrive does not throw here; a transport might
            c = make(SubChannelCode::Status::Failed, {}, e.what());
        }
        unsupported = c.status == SubChannelCode::Status::Unsupported;
        return c;
    };
    codes.mcn = guarded([&] { return drive.readMcn(); });
    for (const Track& t : tracks)
        if (t.isAudio) codes.isrcs[t.number] = guarded([&] { return drive.readIsrc(t.number); });
    return codes;
}

}  // namespace cdr
