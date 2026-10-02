#include "cdreader/toc.h"

#include <cstdio>
#include <stdexcept>

namespace cdr {

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

Toc Toc::parse(const uint8_t* data, size_t length) {
    if (length < 4) throw std::runtime_error("TOC response too short");
    const size_t declared = (size_t(data[0]) << 8 | data[1]) + 2;
    const size_t usable = declared < length ? declared : length;

    Toc toc;
    toc.firstTrack = data[2];
    toc.lastTrack = data[3];

    bool haveLeadOut = false;
    for (size_t off = 4; off + 8 <= usable; off += 8) {
        const uint8_t* d = data + off;
        const uint8_t control = d[1] & 0x0F;
        const int number = d[2];
        const uint32_t lba = be32(d + 4);
        if (number == 0xAA) {
            toc.leadOutLba = lba;
            haveLeadOut = true;
            continue;
        }
        if (number < 1 || number > 99) continue;
        Track t;
        t.number = number;
        t.startLba = lba;
        t.isAudio = (control & 0x04) == 0;
        t.preEmphasis = t.isAudio && (control & 0x01) != 0;
        t.copyPermitted = (control & 0x02) != 0;
        toc.tracks.push_back(t);
    }
    if (toc.tracks.empty() || !haveLeadOut) throw std::runtime_error("TOC has no tracks or no lead-out");

    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        Track& t = toc.tracks[i];
        uint32_t end = (i + 1 < toc.tracks.size()) ? toc.tracks[i + 1].startLba : toc.leadOutLba;
        // Enhanced CD (CD-Extra): the data session follows the audio session,
        // separated by a lead-out/lead-in that is not part of the audio track.
        if (i + 1 < toc.tracks.size() && t.isAudio && !toc.tracks[i + 1].isAudio &&
            end >= t.startLba + kSessionGapSectors) {
            end -= kSessionGapSectors;
        }
        if (end < t.startLba) throw std::runtime_error("TOC track addresses are not ascending");
        t.lengthSectors = end - t.startLba;
    }
    return toc;
}

const Track* Toc::findTrack(int number) const {
    for (const Track& t : tracks)
        if (t.number == number) return &t;
    return nullptr;
}

Toc::LbaRange Toc::audioRange(const Track& track) const {
    LbaRange range{track.startLba, track.endLba()};
    size_t i = 0;
    while (i < tracks.size() && tracks[i].number != track.number) ++i;
    if (i == tracks.size()) return range;
    for (size_t j = i; j > 0 && tracks[j - 1].isAudio; --j) range.begin = tracks[j - 1].startLba;
    for (size_t j = i + 1; j < tracks.size() && tracks[j].isAudio; ++j) range.end = tracks[j].endLba();
    return range;
}

size_t Toc::audioTrackCount() const {
    size_t n = 0;
    for (const Track& t : tracks) n += t.isAudio ? 1 : 0;
    return n;
}

uint32_t Toc::cddbId() const {
    auto digitSum = [](uint32_t v) {
        uint32_t s = 0;
        for (; v > 0; v /= 10) s += v % 10;
        return s;
    };
    uint32_t n = 0;
    for (const Track& t : tracks) n += digitSum((t.startLba + kPregapSectors) / kSectorsPerSecond);
    const uint32_t total = (leadOutLba + kPregapSectors) / kSectorsPerSecond -
                           (tracks.front().startLba + kPregapSectors) / kSectorsPerSecond;
    return ((n % 0xFF) << 24) | (total << 8) | uint32_t(tracks.size());
}

std::string formatMsf(uint32_t sectors) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02u:%02u.%02u", sectors / (60 * kSectorsPerSecond),
                  (sectors / kSectorsPerSecond) % 60, sectors % kSectorsPerSecond);
    return buf;
}

}  // namespace cdr
