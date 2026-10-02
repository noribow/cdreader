#pragma once

#include <cstdint>
#include <vector>

#include "cdreader/metadata.h"

namespace cdr {

// Tag blocks shared by the output formats. Text is written as UTF-8; empty
// fields are omitted. Both functions return an empty vector when there is
// nothing to write.

// RIFF "LIST" chunk of type "INFO" (header included, padded to an even size):
// INAM title, IART artist, IPRD album, ITRK track number, ICRD year,
// IGNR genre, ICMT CDDB disc id.
std::vector<uint8_t> riffInfoChunk(const TrackMetadata& metadata);

// ID3v2.4 tag (UTF-8 text frames, no padding): TIT2, TPE1, TALB, TPE2,
// TRCK "n/total", TDRC, TCON and TXXX "DISCID".
std::vector<uint8_t> id3v2Tag(const TrackMetadata& metadata);

}  // namespace cdr
