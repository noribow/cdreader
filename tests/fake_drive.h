#pragma once

#include <map>
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

    cdr::ScsiResult execute(const uint8_t* cdb, size_t cdbLength, void* data, size_t dataLength,
                            cdr::DataDirection direction, unsigned timeoutSeconds) override;

private:
    cdr::ScsiResult checkCondition(uint8_t key, uint8_t asc, uint8_t ascq);

    std::vector<FakeTrack> tracks_;
    uint32_t leadOut_;
    uint32_t unstableCounter_ = 0;
};
