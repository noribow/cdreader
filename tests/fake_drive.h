#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cdreader/clock.h"
#include "cdreader/scsi.h"
#include "cdreader/toc.h"

// In-memory CD drive speaking just enough MMC for the core library. It is
// also the clock of the cache detection tests: every command advances a
// simulated time (#34).
class FakeDrive : public cdr::ScsiTransport, public cdr::Clock {
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
    int readCommands = 0;  // READ CD of audio without sub-channel data (with or without C2 bits)

    // C2 error pointers (#33). MODE SENSE page 2Ah reports c2Supported; READ
    // CD with error field 01b (294 bytes) / 10b (296 bytes) returns C2 bits,
    // one per byte, MSB first.
    bool modeSenseSupported = true;  // false: ILLEGAL REQUEST
    bool c2Supported = false;
    bool c2ReadsSupported = true;    // false: READ CD with an error field fails (ILLEGAL REQUEST)
    bool c2IgnoresErrorField = false;  // returns audio only (a short transfer)
    struct C2Fault {
        int reads = -1;           // reads affected (-1: every read)
        bool corrupt = true;      // wrong (interpolated) data in the byte range
        bool flagged = true;      // ... reported with C2 bits (false: silently wrong)
        bool varying = true;      // different wrong data on every read (false: the same every time)
        size_t firstByte = 1000;  // affected bytes of the sector
        size_t byteCount = 16;
    };
    std::map<uint32_t, C2Fault> c2Faults;  // by LBA; plain reads get the corrupt data as well
    int c2ReadCommands = 0;
    std::vector<uint8_t> lastAudioCdb;  // last READ CD without sub-channel data

    // Read cache (#34). With cacheSectors > 0, READ CD of audio without
    // sub-channel data keeps what it returned (audio and C2 bits, faults
    // included) for the last cacheSectors sectors read (LRU); a request whose
    // sectors are all cached is answered from the cache: the same data
    // again, no faults counted, and fast. READ(12) with FUA (transfer length
    // 0) empties the cache when fuaHonoured.
    uint32_t cacheSectors = 0;
    bool fuaSupported = true;   // false: READ(12) answers ILLEGAL REQUEST (illegal mode for this track)
    bool fuaHonoured = true;    // false: READ(12) with FUA is accepted and ignored
    uint16_t bufferKB = 0;      // MODE SENSE page 2Ah bytes 12..13
    int fuaCommands = 0;        // READ(12) commands received
    int discReads = 0;          // READ CD of audio served from the disc
    int cacheHits = 0;          // ... served from the cache
    // Simulated time in microseconds (nowMicros()): every command costs
    // commandMicros; a disc read additionally accessMicros (short) or
    // seekMicros (more than seekDistance sectors away) unless it continues
    // where the last disc read ended, plus discSectorMicros per sector
    // (about 13x); a cache hit cachedSectorMicros per sector.
    uint64_t simulatedMicros = 0;
    uint64_t commandMicros = 200;
    uint64_t accessMicros = 6000;
    uint64_t seekMicros = 80000;
    uint32_t seekDistance = 1000;
    uint64_t discSectorMicros = 1000;
    uint64_t cachedSectorMicros = 30;
    uint64_t nowMicros() override { return simulatedMicros; }
    // Every command: opcode, LBA and sector count (READ CD / READ(12); 0 otherwise).
    struct Command {
        uint8_t opcode;
        uint32_t lba;
        uint32_t count;
        bool operator==(const Command& o) const { return opcode == o.opcode && lba == o.lba && count == o.count; }
    };
    std::vector<Command> commandLog;
    std::vector<uint8_t> lastRead12Cdb;
    void clearCache() {
        cache_.clear();
        cacheOrder_.clear();
    }
    bool cached(uint32_t lba) const { return cache_.count(lba) != 0; }

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
    // Drive quirks around the Q sub-channel (#41):
    // The Q frame returned with sector n is the one of sector n + qDelay
    // (READ CD with sub-channel data; its absolute time says n + qDelay).
    int qDelay = 0;
    // Positions whose Q frame is mode 2 / 3 (MCN / ISRC), in addition to otherAdrEvery.
    std::set<uint32_t> otherAdrSectors;
    // Sectors (as requested) for which formatted Q (selection 010b) comes back
    // as 16 zero bytes, while raw P-W and READ SUB-CHANNEL work.
    std::set<uint32_t> formattedQBlank;
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
    uint32_t c2Counter_ = 0;
    uint32_t lastReadLba_ = 0;
    uint32_t head_ = 0;  // where the last disc read ended
    std::map<uint32_t, std::vector<uint8_t>> cache_;  // audio + 296 bytes of C2 bits per sector
    std::deque<uint32_t> cacheOrder_;                 // least recently used first
    void cacheStore(uint32_t lba, const uint8_t* sector);

    int trackAt(uint32_t lba) const;  // index into tracks_
    void subQ(uint32_t lba, uint8_t selection, uint8_t* out);
};
