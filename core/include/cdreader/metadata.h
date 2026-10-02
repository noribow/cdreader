#pragma once

#include <string>
#include <vector>

namespace cdr {

// Descriptive information for one track, e.g. from CDDB (#6), written as
// tags by the output formats (#7, #8). Empty strings mean "unknown".
struct TrackMetadata {
    int trackNumber = 0;
    int trackTotal = 0;
    std::string title;
    std::string artist;       // track artist (falls back to the album artist)
    std::string album;
    std::string albumArtist;
    std::string genre;
    std::string year;
    std::string discId;       // CDDB disc id as 8 hex digits
};

struct AlbumMetadata {
    std::string artist;
    std::string title;
    std::string genre;
    std::string year;
    std::string discId;
    std::vector<std::string> trackTitles;   // index 0 = track 1
    std::vector<std::string> trackArtists;  // optional, same indexing

    // Metadata for track `number` (1-based) of a disc with `total` tracks.
    TrackMetadata forTrack(int number, int total) const;
};

}  // namespace cdr
