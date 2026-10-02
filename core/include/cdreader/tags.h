#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/metadata.h"

namespace cdr {

// Tag blocks shared by the output formats. Text is written as UTF-8; empty
// fields are omitted. Both functions return an empty vector when there is
// nothing to write.

// RIFF "LIST" chunk of type "INFO" (header included, padded to an even size):
// INAM title, IART artist, IPRD album, ITRK track number, ICRD year,
// IGNR genre, ICMT CDDB disc id. (No ISRC / MCN: see tags.cpp.)
std::vector<uint8_t> riffInfoChunk(const TrackMetadata& metadata);

// ID3v2.4 tag (UTF-8 text frames, no padding): TIT2, TPE1, TALB, TPE2,
// TRCK "n/total", TDRC, TCON, TSRC (ISRC), TXXX "DISCID" and TXXX "BARCODE"
// (the disc's MCN, named as MusicBrainz Picard / foobar2000 do).
std::vector<uint8_t> id3v2Tag(const TrackMetadata& metadata);

// Vorbis comment structure (vendor string + "NAME=value" fields, lengths
// little-endian, no framing bit), shared by FLAC (VORBIS_COMMENT block), Ogg
// Opus (OpusTags) and Ogg Vorbis (comment header): TITLE, ARTIST, ALBUM,
// ALBUMARTIST, TRACKNUMBER, TRACKTOTAL, DATE, GENRE, CDDB, ISRC, BARCODE (the
// MCN: a UPC / EAN barcode number; CATALOGNUMBER is the label's catalog
// number in the MusicBrainz / Picard mapping) and, when not empty, CUESHEET (the embedded CUE sheet read by foobar2000 and others).
// Always returns at least the vendor string and the field count.
std::vector<uint8_t> vorbisComment(const TrackMetadata& metadata, const std::string& vendor,
                                   const std::string& cueSheet = {});

}  // namespace cdr
