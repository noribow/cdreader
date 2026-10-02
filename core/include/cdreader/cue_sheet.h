#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/gaps.h"
#include "cdreader/metadata.h"
#include "cdreader/toc.h"

namespace cdr {

// One TRACK entry of a CUE sheet.
struct CueTrack {
    int number = 0;
    std::string file;              // audio file holding the track (name relative to the sheet)
    uint32_t startSectors = 0;     // INDEX 01 position inside `file`, in CD frames (1/75 s)
    bool preEmphasis = false;      // FLAGS PRE
    bool copyPermitted = false;    // FLAGS DCP
    std::string title;
    std::string performer;
    std::string isrc;              // ISRC line (written only when valid, see subchannel.h)

    // Pregap and index points from gap detection (#25, gaps.h).
    bool hasIndex00 = false;       // INDEX 00 line (start of the pregap)
    uint32_t index00Sectors = 0;   // its position inside index00File (or `file` when that is empty)
    // The file holding INDEX 00 when it is not `file`: per-track rips append
    // the pregap to the previous track's file (EAC's default sheet), and the
    // HTOA ripped as track 00 holds the pregap of track 1.
    std::string index00File;
    uint32_t pregapCommandSectors = 0;  // PREGAP line: silence that is in no file (an HTOA not ripped)
    std::vector<uint32_t> laterIndexes; // INDEX 02, 03, ... positions inside `file`
};

// Tracks of a single-file rip: `tracks` written back to back into `file`.
// Titles, performers and ISRCs come from `album`. The tracks must be consecutive on
// the disc (each one starting where the previous one ends), otherwise the
// file would not be an image of the disc: throws std::invalid_argument.
//
// With `gaps` (#25): INDEX 00 of every track whose pregap was detected (the
// pregap is part of the image: the end of the previous track) and INDEX 02+.
// `withHtoa`: the image starts with the HTOA (LBA 0 up to track 1, which must
// be the first track), so track 1 gets INDEX 00 at 00:00:00 and every
// position moves by the HTOA's length. A detected pregap of the first track
// that is not in the image (e.g. -t 3-5) is left out.
std::vector<CueTrack> singleFileCueTracks(const std::vector<Track>& tracks, const std::string& file,
                                          const AlbumMetadata& album, const DiscGaps& gaps = {},
                                          bool withHtoa = false);

// Tracks of a per-track rip: each track in its own file, starting at 00:00:00.
// `files[i]` is the file of `tracks[i]`.
//
// With `gaps` (#25), the sheet is the one EAC writes by default ("gaps
// appended to the previous track", also called non-compliant): the pregap of
// track N is at the end of the file of track N-1, so track N starts there
// with INDEX 00 and its own FILE line follows before INDEX 01 00:00:00. That
// needs track N-1 in `tracks` right before it; otherwise the pregap is left
// out. Track 1 with an HTOA: INDEX 00 at 00:00:00 of `htoaFile` (the HTOA
// ripped as track 00), or a PREGAP line (silence of the same length, keeping
// the disc layout when burning) when `htoaFile` is empty.
std::vector<CueTrack> perTrackCueTracks(const std::vector<Track>& tracks, const std::vector<std::string>& files,
                                        const AlbumMetadata& album, const DiscGaps& gaps = {},
                                        const std::string& htoaFile = {});

// FILE type keyword for an audio file extension: "MP3", "AIFF", otherwise
// "WAVE" (used by EAC / foobar2000 for every non-MP3 audio format, FLAC included).
std::string cueFileType(const std::string& extension);

// CUE sheet text (CRLF line endings, UTF-8). album.mcn becomes the disc's
// CATALOG line, CueTrack::isrc the track's ISRC line (after FLAGS, before
// INDEX, as metaflac writes them); codes that do not validate are left out. A UTF-8 BOM is added only when
// the text contains non-ASCII characters; see cue_sheet.cpp.
std::string formatCueSheet(const AlbumMetadata& album, const std::vector<CueTrack>& tracks);

// A CUE sheet carried inside a single-file (disc image) output, for formats
// that support it (FLAC: CUESHEET metadata block + CUESHEET tag, #16).
struct EmbeddedCueSheet {
    // INDEX positions relative to the start of the file; INDEX 00 only when
    // inside the file (index00File empty).
    std::vector<CueTrack> tracks;
    std::string mcn;               // media catalog number (13 digits) or empty
    uint32_t totalSectors = 0;     // length of the image: position of the lead-out
    std::string text;              // the CUE sheet as text, as written to the .cue file
};

// "mm:ss:ff" as used by INDEX lines (minutes may exceed 99).
std::string formatCueTime(uint32_t sectors);

}  // namespace cdr
