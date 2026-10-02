#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cdr {

constexpr uint32_t kSectorBytes = 2352;       // one CD-DA frame block
constexpr uint32_t kSectorsPerSecond = 75;
constexpr uint32_t kPregapSectors = 150;      // 2 seconds before LBA 0
constexpr uint32_t kSessionGapSectors = 11400; // lead-out + lead-in + pregap between sessions

struct Track {
    int number = 0;
    uint32_t startLba = 0;
    uint32_t lengthSectors = 0;
    bool isAudio = true;
    bool preEmphasis = false;
    bool copyPermitted = false;

    uint32_t endLba() const { return startLba + lengthSectors; }  // exclusive
};

struct Toc {
    int firstTrack = 0;
    int lastTrack = 0;
    uint32_t leadOutLba = 0;
    std::vector<Track> tracks;

    // Parses a READ TOC/PMA/ATIP response (format 0000b, LBA addressing).
    // Throws std::runtime_error on malformed data.
    static Toc parse(const uint8_t* data, size_t length);

    const Track* findTrack(int number) const;
    size_t audioTrackCount() const;

    // freedb/CDDB disc id.
    uint32_t cddbId() const;
};

// Formats a sector count as "mm:ss.ff" (ff = 1/75 s frames).
std::string formatMsf(uint32_t sectors);

}  // namespace cdr
