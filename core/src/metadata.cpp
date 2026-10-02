#include "cdreader/metadata.h"

namespace cdr {

TrackMetadata AlbumMetadata::forTrack(int number, int total) const {
    TrackMetadata m;
    m.trackNumber = number;
    m.trackTotal = total;
    m.album = title;
    m.albumArtist = artist;
    m.artist = artist;
    m.genre = genre;
    m.year = year;
    m.discId = discId;
    m.mcn = mcn;
    const size_t i = size_t(number - 1);
    if (number >= 1 && i < trackTitles.size()) m.title = trackTitles[i];
    if (number >= 1 && i < trackIsrcs.size()) m.isrc = trackIsrcs[i];
    if (number >= 1 && i < trackArtists.size() && !trackArtists[i].empty()) m.artist = trackArtists[i];
    return m;
}

}  // namespace cdr
