#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "cdreader/scsi.h"
#include "cdreader/toc.h"

// In-memory CD drive speaking just enough MMC for the core library.
class FakeDrive : public cdr::ScsiTransport {
public:
    struct FakeTrack {
        uint32_t startLba;
        bool data;
    };

    FakeDrive(std::vector<FakeTrack> tracks, uint32_t leadOut) : tracks_(std::move(tracks)), leadOut_(leadOut) {}

    // Deterministic sample byte for a position on the disc.
    static uint8_t sampleByte(uint32_t lba, size_t offset) {
        return uint8_t((lba * 131u + offset * 7u + (offset >> 8)) & 0xFF);
    }

    // The sector fails this many reads before returning good data; -1 = never readable.
    std::map<uint32_t, int> failuresBySector;
    // The sector returns different data on every read (simulates a scratched area).
    std::map<uint32_t, bool> unstableSectors;
    bool discPresent = true;
    int readCommands = 0;

    // READ SUB-CHANNEL (42h), formats 02h / 03h. A code that is set is
    // returned with MCVal / TCVal = 1 as is (up to 13 / 12 bytes, so that
    // malformed codes can be tested); an unset one with the valid bit 0.
    std::string mcn;
    std::map<int, std::string> isrcs;  // by track number
    bool subChannelSupported = true;   // false: ILLEGAL REQUEST (invalid opcode)
    size_t subChannelTransferLimit = SIZE_MAX;  // fewer bytes transferred (short response)
    int subChannelCommands = 0;
    std::vector<uint8_t> lastSubChannelCdb;

    // Q sub-channel of every sector (READ CD with sub-channel data, READ
    // SUB-CHANNEL current position; #25). Track N's INDEX 00 starts
    // pregaps[N] sectors before its TOC start (inside the previous track);
    // track 1 starting after LBA 0 has an HTOA (INDEX 00 from LBA 0).
    std::map<int, uint32_t> pregaps;
    std::map<int, std::vector<uint32_t>> laterIndexes;  // LBAs of INDEX 02, 03, ... by track
    bool formattedQSupported = true;      // READ CD sub-channel selection 010b
    bool formattedQCrc = true;            // false: formatted Q with CRC bytes 0 (not reported)
    bool rawSubChannelSupported = true;   // READ CD sub-channel selection 001b
    bool currentPositionSupported = true; // READ SUB-CHANNEL format 01h
    // n > 0: sectors with lba % n == n / 2 carry mode 2 / mode 3 (MCN / ISRC)
    // frames instead of the position, alternately.
    uint32_t otherAdrEvery = 0;
    // The sector's Q frame is corrupt this many reads (-1: always): bit errors,
    // so the CRC fails (formatted Q without CRC: unparsable BCD).
    std::map<uint32_t, int> badQ;
    // The sector's Q frame claims the wrong side of the track boundary this
    // many reads, with the CRC of the right frame (caught by the CRC, not
    // when the drive reports none).
    std::map<uint32_t, int> wrongQ;
    int subQReads = 0;  // READ CD commands with sub-channel data (not counted in readCommands)
    std::vector<uint8_t> lastReadCdCdb;

    // The 12 Q bytes (with CRC) of a sector as recorded on the disc.
    void qFrame(uint32_t lba, uint8_t q[12]) const;
    bool isPause(uint32_t lba) const;  // INDEX 00

    cdr::ScsiResult execute(const uint8_t* cdb, size_t cdbLength, void* data, size_t dataLength,
                            cdr::DataDirection direction, unsigned timeoutSeconds) override;

private:
    cdr::ScsiResult checkCondition(uint8_t key, uint8_t asc, uint8_t ascq);

    std::vector<FakeTrack> tracks_;
    uint32_t leadOut_;
    uint32_t unstableCounter_ = 0;
    uint32_t lastReadLba_ = 0;

    int trackAt(uint32_t lba) const;  // index into tracks_
    void subQ(uint32_t lba, uint8_t selection, uint8_t* out);
};
