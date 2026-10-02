#include "fake_drive.h"

#include <algorithm>
#include <cstring>

cdr::ScsiResult FakeDrive::checkCondition(uint8_t key, uint8_t asc, uint8_t ascq) {
    cdr::ScsiResult r;
    r.transportOk = true;
    r.status = 0x02;
    r.sense = {key, asc, ascq};
    return r;
}

cdr::ScsiResult FakeDrive::execute(const uint8_t* cdb, size_t, void* data, size_t dataLength,
                                   cdr::DataDirection, unsigned) {
    uint8_t* out = static_cast<uint8_t*>(data);
    cdr::ScsiResult ok;
    ok.transportOk = true;
    ok.transferred = dataLength;

    switch (cdb[0]) {
        case 0x00:  // TEST UNIT READY
            return discPresent ? ok : checkCondition(0x2, 0x3A, 0x00);

        case 0x12: {  // INQUIRY
            std::memset(out, ' ', dataLength);
            std::memcpy(out + 8, "FAKE", 4);
            std::memcpy(out + 16, "CD-ROM DRIVE", 12);
            std::memcpy(out + 32, "1.00", 4);
            return ok;
        }

        case 0x43: {  // READ TOC
            if (!discPresent) return checkCondition(0x2, 0x3A, 0x00);
            std::memset(out, 0, dataLength);
            const size_t entries = tracks_.size() + 1;
            const size_t length = 2 + 8 * entries;
            out[0] = uint8_t(length >> 8);
            out[1] = uint8_t(length);
            out[2] = 1;
            out[3] = uint8_t(tracks_.size());
            for (size_t i = 0; i < entries; ++i) {
                uint8_t* d = out + 4 + 8 * i;
                const bool leadOut = i == tracks_.size();
                const uint32_t lba = leadOut ? leadOut_ : tracks_[i].startLba;
                d[1] = 0x10 | (!leadOut && tracks_[i].data ? 0x04 : 0x00);
                d[2] = leadOut ? 0xAA : uint8_t(i + 1);
                d[4] = uint8_t(lba >> 24);
                d[5] = uint8_t(lba >> 16);
                d[6] = uint8_t(lba >> 8);
                d[7] = uint8_t(lba);
            }
            return ok;
        }

        case 0x42: {  // READ SUB-CHANNEL
            ++subChannelCommands;
            lastSubChannelCdb.assign(cdb, cdb + 10);
            if (!subChannelSupported) return checkCondition(0x5, 0x20, 0x00);
            if (!discPresent) return checkCondition(0x2, 0x3A, 0x00);
            const uint8_t format = cdb[3];
            const int track = cdb[6];
            if (!(cdb[2] & 0x40) || (format != 0x02 && format != 0x03)) return checkCondition(0x5, 0x24, 0x00);
            if (format == 0x03 && (track < 1 || track > int(tracks_.size()))) return checkCondition(0x5, 0x24, 0x00);
            uint8_t response[24] = {};
            response[1] = 0x15;  // audio status: no current audio status
            response[3] = 20;    // sub-channel data length
            response[4] = format;
            const std::string* code = nullptr;
            if (format == 0x02) {
                if (!mcn.empty()) code = &mcn;
            } else {
                response[5] = 0x30 | (tracks_[size_t(track - 1)].data ? 0x04 : 0x00);  // ADR 3, control
                response[6] = uint8_t(track);
                auto it = isrcs.find(track);
                if (it != isrcs.end()) code = &it->second;
            }
            if (code) {
                response[8] = 0x80;  // MCVal / TCVal
                std::memcpy(response + 9, code->data(), std::min(code->size(), size_t(format == 0x02 ? 13 : 12)));
            }
            const size_t allocation = size_t(cdb[7]) << 8 | cdb[8];
            const size_t n = std::min({sizeof response, allocation, dataLength, subChannelTransferLimit});
            std::memcpy(out, response, n);
            ok.transferred = n;
            return ok;
        }

        case 0xBE: {  // READ CD
            ++readCommands;
            const uint32_t lba = (uint32_t(cdb[2]) << 24) | (uint32_t(cdb[3]) << 16) | (uint32_t(cdb[4]) << 8) | cdb[5];
            const uint32_t count = (uint32_t(cdb[6]) << 16) | (uint32_t(cdb[7]) << 8) | cdb[8];
            if (dataLength != size_t(count) * cdr::kSectorBytes) return checkCondition(0x5, 0x24, 0x00);
            if (lba + count > leadOut_) return checkCondition(0x5, 0x21, 0x00);
            for (uint32_t s = lba; s < lba + count; ++s) {
                auto it = failuresBySector.find(s);
                if (it != failuresBySector.end() && it->second != 0) {
                    if (it->second > 0) --it->second;
                    return checkCondition(0x3, 0x11, 0x05);  // L-EC uncorrectable error
                }
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t s = lba + i;
                uint8_t* sector = out + size_t(i) * cdr::kSectorBytes;
                for (size_t b = 0; b < cdr::kSectorBytes; ++b) sector[b] = sampleByte(s, b);
                if (unstableSectors.count(s)) sector[0] = uint8_t(++unstableCounter_);
            }
            return ok;
        }
    }
    return checkCondition(0x5, 0x20, 0x00);  // invalid command operation code
}
