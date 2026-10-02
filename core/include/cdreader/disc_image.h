#pragma once

// Single-file rips (#16, #25, #42): the tracks of a disc written back to back
// into one file (an image of the disc) with a CUE sheet. Shared by the
// Windows CLI (rip --single-file) and the Android app, so that both write the
// same file names, CUE sheets, embedded CUE sheets and rip.log lines.

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/gaps.h"
#include "cdreader/metadata.h"
#include "cdreader/toc.h"

namespace cdr {

struct DiscImagePlan {
    std::string fileName;     // "<Artist> - <Album>.<ext>", or "CDImage.<ext>" without metadata
    std::string cueFileName;  // the same base name with ".cue"
    bool withHtoa = false;    // the image starts with the HTOA (LBA 0 up to track 1)
    // What to rip into the image, in order: the HTOA (htoaTrack(), number 0)
    // first when included, then the tracks.
    std::vector<Track> parts;
    std::vector<CueTrack> cueTracks;  // singleFileCueTracks() of the tracks, FILE = fileName
    uint32_t totalSectors = 0;        // length of the image (the lead-out position in it)
    std::string cueSheet;             // formatCueSheet(album, cueTracks): the external .cue file
    std::string mcn;                  // album.mcn (CATALOG)

    // The CUE sheet for writers that carry it inside the image
    // (AudioWriter::canEmbedCueSheet(): FLAC, Ogg FLAC, Matroska).
    EmbeddedCueSheet embeddedCueSheet() const;

    // Name of a part in rip.log and progress output: "Track 01", "Track 00 (HTOA)".
    static std::string partName(const Track& part);
};

// Plans the image of `tracks` (audio tracks of `toc`, in disc order) for a
// format with file name extension `extension`. `includeHtoa`: put the HTOA
// in front when the disc has one (gaps.hasHtoa()) and `tracks` starts with
// track 1; the CLI and the app always include it. Names come from
// albumFileBase(album, fallbackBase). Throws std::invalid_argument when the
// tracks are not consecutive on the disc (see singleFileCueTracks()) or empty.
DiscImagePlan planDiscImage(const std::vector<Track>& tracks, const Toc& toc, const AlbumMetadata& album,
                            const DiscGaps& gaps, bool includeHtoa, const std::string& extension,
                            const std::string& fallbackBase = "CDImage");

// What a writer embeds for the "Embedded CUE sheet:" line of rip.log:
// "CUESHEET block and tag" (FLAC, Ogg FLAC), "Matroska chapters (one per
// track[, plus the HTOA])" (extension "mka").
std::string embeddedCueSheetDescription(const std::string& extension, bool withHtoa);

// rip.log line of the whole image: "<fileName>  CRC32 <crc of all its PCM>".
std::string discImageCrcLogLine(const std::string& fileName, uint32_t crc32);

}  // namespace cdr
