#include "cdreader/disc_image.h"

#include <cstdio>
#include <stdexcept>

#include "cdreader/file_naming.h"

namespace cdr {

EmbeddedCueSheet DiscImagePlan::embeddedCueSheet() const {
    EmbeddedCueSheet embedded;
    embedded.tracks = cueTracks;
    embedded.mcn = mcn;
    embedded.totalSectors = totalSectors;
    embedded.text = cueSheet;
    return embedded;
}

std::string DiscImagePlan::partName(const Track& part) {
    char base[32];
    std::snprintf(base, sizeof base, part.number == 0 ? "Track %02d (HTOA)" : "Track %02d", part.number);
    return base;
}

DiscImagePlan planDiscImage(const std::vector<Track>& tracks, const Toc& toc, const AlbumMetadata& album,
                            const DiscGaps& gaps, bool includeHtoa, const std::string& extension,
                            const std::string& fallbackBase) {
    if (tracks.empty()) throw std::invalid_argument("no tracks");
    DiscImagePlan plan;
    const std::string base = albumFileBase(album, fallbackBase);
    plan.fileName = base + "." + extension;
    plan.cueFileName = base + ".cue";
    plan.withHtoa = includeHtoa && gaps.hasHtoa() && tracks.front().number == 1;
    plan.cueTracks = singleFileCueTracks(tracks, plan.fileName, album, gaps, plan.withHtoa);
    plan.parts = tracks;
    if (plan.withHtoa) plan.parts.insert(plan.parts.begin(), htoaTrack(toc));
    for (const Track& t : tracks) plan.totalSectors += t.lengthSectors;
    if (plan.withHtoa) plan.totalSectors += gaps.htoaSectors;
    plan.cueSheet = formatCueSheet(album, plan.cueTracks);
    plan.mcn = album.mcn;
    return plan;
}

std::string embeddedCueSheetDescription(const std::string& extension, bool withHtoa) {
    // FLAC: CUESHEET block + tag; Matroska: chapters (#23).
    return extension != "mka" ? "CUESHEET block and tag"
           : withHtoa         ? "Matroska chapters (one per track, plus the HTOA)"
                              : "Matroska chapters (one per track)";
}

std::string discImageCrcLogLine(const std::string& fileName, uint32_t crc32) {
    char crc[16];
    std::snprintf(crc, sizeof crc, "%08X", crc32);
    return fileName + "  CRC32 " + crc;
}

}  // namespace cdr
