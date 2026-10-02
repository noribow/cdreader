#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "cdreader/scsi.h"
#include "cdreader/subchannel.h"
#include "cdreader/toc.h"

namespace cdr {

// 26 * 2352 = 61,152 bytes: stays below the common 64 KiB transfer limit.
constexpr uint32_t kMaxSectorsPerRead = 26;

struct DriveInfo {
    std::string vendor;
    std::string product;
    std::string revision;

    std::string displayName() const;
};

class ScsiError : public std::runtime_error {
public:
    ScsiError(const std::string& what, ScsiResult result)
        : std::runtime_error(what + ": " + result.describe()), result_(std::move(result)) {}
    const ScsiResult& result() const { return result_; }

private:
    ScsiResult result_;
};

// MMC command layer on top of a ScsiTransport.
class CdDrive {
public:
    explicit CdDrive(ScsiTransport& transport) : transport_(transport) {}

    DriveInfo inquiry();          // throws ScsiError
    bool isReady();
    Toc readToc();                // throws ScsiError / std::runtime_error

    // Reads `count` CD-DA sectors (count * kSectorBytes bytes) with READ CD.
    // Does not throw: the caller decides how to retry.
    ScsiResult readAudio(uint32_t lba, uint32_t count, uint8_t* out);

    // READ CD of `count` CD-DA sectors with sub-channel data (#25): each sector
    // is 2352 bytes of audio followed by subChannelBytesPerSector(selection)
    // bytes. Does not throw; a drive that does not support the selection
    // answers ILLEGAL REQUEST.
    ScsiResult readAudioWithSubChannel(uint32_t lba, uint32_t count, SubChannelSelection selection, uint8_t* out);

    // READ SUB-CHANNEL (42h) with SubQ = 1: `length` bytes (at most 65535) of
    // Q sub-channel data in `format` into `out`; `track` selects the track
    // for the ISRC format (0 otherwise), `msf` the address format of the
    // current-position format. Does not throw. A response shorter than
    // requested is not an error here: `transferred` tells the caller.
    ScsiResult readSubChannel(SubChannelFormat format, int track, uint8_t* out, size_t length, bool msf = false);

    // The media catalog number / the ISRC of an audio track (1..99). Never
    // throw: the status says why there is no code (see subchannel.h).
    SubChannelCode readMcn();
    SubChannelCode readIsrc(int track);

private:
    ScsiTransport& transport_;
};

}  // namespace cdr
