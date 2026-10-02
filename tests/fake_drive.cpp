#include "fake_drive.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "cdreader/cd_drive.h"
#include "cdreader/subchannel.h"

cdr::ScsiResult FakeDrive::checkCondition(uint8_t key, uint8_t asc, uint8_t ascq) {
    cdr::ScsiResult r;
    r.transportOk = true;
    r.status = 0x02;
    r.sense = {key, asc, ascq};
    return r;
}

namespace {

uint8_t bcd(uint32_t v) { return uint8_t((v / 10) << 4 | (v % 10)); }

void putMsf(uint8_t* p, uint32_t sectors) {
    p[0] = bcd(sectors / (60 * cdr::kSectorsPerSecond));
    p[1] = bcd(sectors / cdr::kSectorsPerSecond % 60);
    p[2] = bcd(sectors % cdr::kSectorsPerSecond);
}

void putBe32(uint8_t* p, int32_t v) {
    const uint32_t u = uint32_t(v);
    p[0] = uint8_t(u >> 24);
    p[1] = uint8_t(u >> 16);
    p[2] = uint8_t(u >> 8);
    p[3] = uint8_t(u);
}

void setCrc(uint8_t q[12]) {
    const uint16_t crc = uint16_t(cdr::subQCrc16(q, 10) ^ 0xFFFF);
    q[10] = uint8_t(crc >> 8);
    q[11] = uint8_t(crc);
}

// Counts down a per-sector fault; true while it applies.
bool fault(std::map<uint32_t, int>& faults, uint32_t lba) {
    auto it = faults.find(lba);
    if (it == faults.end() || it->second == 0) return false;
    if (it->second > 0) --it->second;
    return true;
}

}  // namespace

void FakeDrive::cacheStore(uint32_t lba, const uint8_t* sector) {
    auto it = cache_.find(lba);
    if (it != cache_.end()) {
        if (it->second.data() != sector) it->second.assign(sector, sector + cdr::kSectorBytes + 296);
        cacheOrder_.erase(std::find(cacheOrder_.begin(), cacheOrder_.end(), lba));
    } else {
        cache_[lba].assign(sector, sector + cdr::kSectorBytes + 296);
    }
    cacheOrder_.push_back(lba);
    while (cacheOrder_.size() > cacheSectors) {
        cache_.erase(cacheOrder_.front());
        cacheOrder_.pop_front();
    }
}

int FakeDrive::trackAt(uint32_t lba) const {
    int owner = 0;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const int number = int(i + 1);
        auto it = pregaps.find(number);
        uint32_t index00 = tracks_[i].startLba;
        if (i == 0) index00 = 0;  // track 1: its pregap reaches back to LBA 0 (HTOA when it starts later)
        else if (it != pregaps.end()) index00 = tracks_[i].startLba - std::min(it->second, tracks_[i].startLba);
        if (lba >= index00) owner = int(i);
    }
    return owner;
}

bool FakeDrive::isPause(uint32_t lba) const { return lba < leadOut_ && lba < tracks_[size_t(trackAt(lba))].startLba; }

void FakeDrive::qFrame(uint32_t lba, uint8_t q[12]) const {
    std::memset(q, 0, 12);
    if (lba >= leadOut_) {
        q[0] = 0x01;
        q[1] = 0xAA;
        q[2] = 0x01;
        putMsf(q + 3, lba - leadOut_);
        putMsf(q + 7, lba + cdr::kPregapSectors);
        setCrc(q);
        return;
    }
    const int i = trackAt(lba);
    const FakeTrack& t = tracks_[size_t(i)];
    const int number = i + 1;
    const uint8_t control = t.data ? 0x4 : 0x0;
    uint32_t index = 0;
    if (lba >= t.startLba) {
        index = 1;
        auto it = laterIndexes.find(number);
        if (it != laterIndexes.end())
            for (uint32_t at : it->second) index += lba >= at ? 1 : 0;
    }
    if (otherAdrEvery > 0 && lba % otherAdrEvery == otherAdrEvery / 2) {
        // Mode 2 (MCN digits) / mode 3 (ISRC): no position, only AFRAME in byte 9.
        const bool isrc = (lba / otherAdrEvery) % 2 == 1;
        q[0] = uint8_t(control << 4 | (isrc ? 3 : 2));
        for (int b = 1; b < 9; ++b) q[b] = uint8_t(0x49 + b);
        q[9] = bcd((lba + cdr::kPregapSectors) % cdr::kSectorsPerSecond);
        setCrc(q);
        return;
    }
    q[0] = uint8_t(control << 4 | 1);
    q[1] = bcd(uint32_t(number));
    q[2] = bcd(index);
    putMsf(q + 3, index == 0 ? t.startLba - lba : lba - t.startLba);
    putMsf(q + 7, lba + cdr::kPregapSectors);
    setCrc(q);
}

// Sub-channel data of one sector for a READ CD selection (1 raw P-W, 2 formatted Q).
void FakeDrive::subQ(uint32_t lba, uint8_t selection, uint8_t* out) {
    uint8_t q[12];
    qFrame(lba, q);
    const bool corrupt = fault(badQ, lba);
    if (fault(wrongQ, lba) && (q[0] & 0x0F) == 1) {
        // The other side of the boundary: INDEX 00 of the next track, or the
        // previous track's INDEX 01; the CRC stays that of the right frame.
        const int number = (q[1] >> 4) * 10 + (q[1] & 0x0F);
        if (q[2] == 0) {
            q[1] = bcd(uint32_t(std::max(1, number - 1)));
            q[2] = 0x01;
        } else {
            q[1] = bcd(uint32_t(std::min(99, number + 1)));
            q[2] = 0x00;
        }
    }
    if (corrupt) q[7] ^= 0x10;  // bit error: the CRC no longer matches
    if (selection == 2) {
        std::memset(out, 0, 16);
        std::memcpy(out, q, formattedQCrc ? 12 : 10);
        if (corrupt && !formattedQCrc) out[4] = 0xFF;  // without CRC: an invalid BCD byte
    } else {
        // Noise in the R-W bits, which the de-interleaving must ignore.
        for (size_t j = 0; j < 96; ++j) out[j] = uint8_t((lba * 37u + j * 11u) & 0x3F);
        cdr::interleaveQ(q, isPause(lba), out);
    }
}

cdr::ScsiResult FakeDrive::execute(const uint8_t* cdb, size_t, void* data, size_t dataLength,
                                   cdr::DataDirection, unsigned) {
    uint8_t* out = static_cast<uint8_t*>(data);
    cdr::ScsiResult ok;
    ok.transportOk = true;
    ok.transferred = dataLength;
    simulatedMicros += commandMicros;
    commandLog.push_back({cdb[0], 0, 0});

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
            if (!(cdb[2] & 0x40) || format < 0x01 || format > 0x03) return checkCondition(0x5, 0x24, 0x00);
            if (format == 0x01) {
                // Current position: the first sector of the last READ CD (LBA addressing).
                if (!currentPositionSupported) return checkCondition(0x5, 0x24, 0x00);
                uint8_t q[12];
                qFrame(lastReadLba_, q);
                uint8_t response[16] = {};
                response[1] = 0x15;
                response[3] = 12;
                response[4] = 0x01;
                response[5] = uint8_t((q[0] & 0x0F) << 4 | q[0] >> 4);  // ADR / CONTROL
                if ((q[0] & 0x0F) == 1) {
                    const int track = trackAt(lastReadLba_);
                    response[6] = uint8_t(track + 1);
                    response[7] = uint8_t((q[2] >> 4) * 10 + (q[2] & 0x0F));
                    putBe32(response + 8, int32_t(lastReadLba_));
                    putBe32(response + 12, int32_t(lastReadLba_) - int32_t(tracks_[size_t(track)].startLba));
                }
                const size_t allocation = size_t(cdb[7]) << 8 | cdb[8];
                const size_t n = std::min({sizeof response, allocation, dataLength, subChannelTransferLimit});
                std::memcpy(out, response, n);
                ok.transferred = n;
                return ok;
            }
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

        case 0x5A: {  // MODE SENSE(10)
            if (!modeSenseSupported) return checkCondition(0x5, 0x20, 0x00);
            if ((cdb[2] & 0x3F) != 0x2A) return checkCondition(0x5, 0x24, 0x00);
            uint8_t response[8 + 28] = {};
            response[1] = sizeof response - 2;  // mode data length
            uint8_t* page = response + 8;       // DBD: no block descriptors
            page[0] = 0x2A;
            page[1] = 26;
            page[5] = uint8_t(0x01 | 0x02 | (c2Supported ? 0x10 : 0x00));  // CD-DA commands, accurate stream, C2
            page[12] = uint8_t(bufferKB >> 8);
            page[13] = uint8_t(bufferKB);
            const size_t allocation = size_t(cdb[7]) << 8 | cdb[8];
            const size_t n = std::min({sizeof response, allocation, dataLength});
            std::memcpy(out, response, n);
            ok.transferred = n;
            return ok;
        }

        case 0xA8: {  // READ(12): only as the FUA cache flush (transfer length 0, no data)
            ++fuaCommands;
            lastRead12Cdb.assign(cdb, cdb + 12);
            const uint32_t lba = (uint32_t(cdb[2]) << 24) | (uint32_t(cdb[3]) << 16) | (uint32_t(cdb[4]) << 8) | cdb[5];
            const uint32_t count = (uint32_t(cdb[6]) << 24) | (uint32_t(cdb[7]) << 16) | (uint32_t(cdb[8]) << 8) | cdb[9];
            commandLog.back().lba = lba;
            commandLog.back().count = count;
            if (!fuaSupported) return checkCondition(0x5, 0x64, 0x00);  // illegal mode for this track
            if (count != 0 || dataLength != 0) return checkCondition(0x5, 0x24, 0x00);
            if (lba >= leadOut_) return checkCondition(0x5, 0x21, 0x00);
            if ((cdb[1] & 0x08) && fuaHonoured) clearCache();
            ok.transferred = 0;
            return ok;
        }

        case 0xBE: {  // READ CD
            const uint8_t selection = cdb[10] & 0x07;
            const bool userData = (cdb[9] & 0x10) != 0;
            const uint8_t errorField = (cdb[9] >> 1) & 0x03;
            if (selection != 0) {
                ++subQReads;
                lastReadCdCdb.assign(cdb, cdb + 12);
                if (selection > 2 || (selection == 2 && !formattedQSupported) ||
                    (selection == 1 && !rawSubChannelSupported))
                    return checkCondition(0x5, 0x24, 0x00);
            } else {
                ++readCommands;
                lastAudioCdb.assign(cdb, cdb + 12);
            }
            if (errorField != 0) {
                ++c2ReadCommands;
                if (!c2ReadsSupported || errorField == 3 || selection != 0) return checkCondition(0x5, 0x24, 0x00);
            }
            const uint32_t lba = (uint32_t(cdb[2]) << 24) | (uint32_t(cdb[3]) << 16) | (uint32_t(cdb[4]) << 8) | cdb[5];
            const uint32_t count = (uint32_t(cdb[6]) << 16) | (uint32_t(cdb[7]) << 8) | cdb[8];
            const size_t subBytes = selection == 1 ? 96 : selection == 2 ? 16 : 0;
            const size_t c2Bytes = errorField == 1 ? cdr::kC2BytesPerSector : errorField == 2 ? 296 : 0;
            const size_t perSector = (userData ? cdr::kSectorBytes : 0) + subBytes + c2Bytes;
            if (dataLength != size_t(count) * perSector) return checkCondition(0x5, 0x24, 0x00);
            if (lba + count > leadOut_) return checkCondition(0x5, 0x21, 0x00);
            commandLog.back().lba = lba;
            commandLog.back().count = count;

            const bool cacheable = selection == 0 && userData && cacheSectors > 0;
            bool hit = cacheable && count > 0;
            for (uint32_t s = lba; hit && s < lba + count; ++s) hit = cache_.count(s) != 0;
            if (hit) {
                ++cacheHits;
                simulatedMicros += uint64_t(count) * cachedSectorMicros;
                for (uint32_t i = 0; i < count; ++i) {
                    cacheStore(lba + i, cache_[lba + i].data());  // most recently used
                    const std::vector<uint8_t>& cachedSector = cache_[lba + i];
                    std::memcpy(out + size_t(i) * perSector, cachedSector.data(), cdr::kSectorBytes + c2Bytes);
                }
                lastReadLba_ = lba;
                if (errorField != 0 && c2IgnoresErrorField) ok.transferred = size_t(count) * cdr::kSectorBytes;
                return ok;
            }
            if (selection == 0) ++discReads;
            if (lba != head_) {
                const int64_t distance = std::abs(int64_t(lba) - int64_t(head_));
                simulatedMicros += distance > int64_t(seekDistance) ? seekMicros : accessMicros;
            }
            simulatedMicros += uint64_t(count) * discSectorMicros;
            head_ = lba + count;

            for (uint32_t s = lba; s < lba + count; ++s) {
                auto it = failuresBySector.find(s);
                if (it != failuresBySector.end() && it->second != 0) {
                    if (it->second > 0) --it->second;
                    return checkCondition(0x3, 0x11, 0x05);  // L-EC uncorrectable error
                }
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t s = lba + i;
                uint8_t* sector = out + size_t(i) * perSector;
                if (userData) {
                    for (size_t b = 0; b < cdr::kSectorBytes; ++b) sector[b] = sampleByte(s, b);
                    if (unstableSectors.count(s)) sector[0] = uint8_t(++unstableCounter_);
                }
                if (selection != 0) subQ(s, selection, sector + (userData ? cdr::kSectorBytes : 0));
                if (selection != 0 || !userData) continue;
                uint8_t full[cdr::kSectorBytes + 296] = {};  // audio + C2 bits as error field 10b returns them
                auto it = c2Faults.find(s);
                if (it != c2Faults.end() && it->second.reads != 0) {
                    C2Fault& f = it->second;
                    if (f.reads > 0) --f.reads;
                    const uint8_t noise = f.varying ? uint8_t(++c2Counter_ % 255 + 1) : uint8_t(0x55);
                    const size_t end = std::min(size_t(cdr::kSectorBytes), f.firstByte + f.byteCount);
                    for (size_t b = f.firstByte; b < end; ++b) {
                        if (f.corrupt) sector[b] ^= noise;
                        if (f.flagged) full[cdr::kSectorBytes + (b >> 3)] |= uint8_t(0x80 >> (b & 7));
                    }
                    if (f.flagged) full[cdr::kSectorBytes + 294] = 0xFF;  // block error byte
                }
                std::memcpy(full, sector, cdr::kSectorBytes);
                if (c2Bytes) std::memcpy(sector + cdr::kSectorBytes, full + cdr::kSectorBytes, c2Bytes);
                if (cacheable) cacheStore(s, full);
            }
            lastReadLba_ = lba;
            if (errorField != 0 && c2IgnoresErrorField) ok.transferred = size_t(count) * cdr::kSectorBytes;
            return ok;
        }
    }
    return checkCondition(0x5, 0x20, 0x00);  // invalid command operation code
}
