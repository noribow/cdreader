#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cdr {

constexpr uint32_t kSectorBytes = 2352;       // one CD-DA frame block
constexpr uint32_t kBytesPerSample = 4;       // 16-bit stereo
constexpr uint32_t kSamplesPerSector = kSectorBytes / kBytesPerSample;  // 588
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

    struct LbaRange {
        uint32_t begin = 0;  // inclusive
        uint32_t end = 0;    // exclusive
    };

    const Track* findTrack(int number) const;

    // The run of consecutive audio tracks containing `track`: the area that
    // can be read as CD-DA around it (used for read offset correction),
    // from LBA 0 when it includes the first track (an HTOA before it is
    // audio, see gaps.h). A
    // track that is not in the TOC (the HTOA, track 0) belongs to the run of
    // the track starting where it ends.
    LbaRange audioRange(const Track& track) const;
    size_t audioTrackCount() const;

    // freedb/CDDB disc id.
    uint32_t cddbId() const;
};

// Formats a sector count as "mm:ss.ff" (ff = 1/75 s frames).
std::string formatMsf(uint32_t sectors);

}  // namespace cdr
