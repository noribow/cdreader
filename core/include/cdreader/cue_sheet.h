#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
};

// Tracks of a single-file rip: `tracks` written back to back into `file`.
// Titles and performers come from `album`. The tracks must be consecutive on
// the disc (each one starting where the previous one ends), otherwise the
// file would not be an image of the disc: throws std::invalid_argument.
std::vector<CueTrack> singleFileCueTracks(const std::vector<Track>& tracks, const std::string& file,
                                          const AlbumMetadata& album);

// Tracks of a per-track rip: each track in its own file, starting at 00:00:00.
// `files[i]` is the file of `tracks[i]`.
std::vector<CueTrack> perTrackCueTracks(const std::vector<Track>& tracks, const std::vector<std::string>& files,
                                        const AlbumMetadata& album);

// FILE type keyword for an audio file extension: "MP3", "AIFF", otherwise
// "WAVE" (used by EAC / foobar2000 for every non-MP3 audio format, FLAC included).
std::string cueFileType(const std::string& extension);

// CUE sheet text (CRLF line endings, UTF-8). A UTF-8 BOM is added only when
// the text contains non-ASCII characters; see cue_sheet.cpp.
std::string formatCueSheet(const AlbumMetadata& album, const std::vector<CueTrack>& tracks);

// "mm:ss:ff" as used by INDEX lines (minutes may exceed 99).
std::string formatCueTime(uint32_t sectors);

}  // namespace cdr
