#pragma once

#include <string>

#include "cdreader/metadata.h"

namespace cdr {

// Makes `name` usable as a single file or directory name on Windows and
// Android: characters invalid there (<>:"/\|?* and control characters) become
// '_', trailing dots and spaces are removed, reserved device names (CON, NUL,
// COM1, ...) get a '_' prefix and overlong names are cut at a UTF-8 character
// boundary. Non-ASCII text such as Japanese is kept. Returns an empty string
// when nothing usable is left.
std::string sanitizeFileName(const std::string& name, size_t maxBytes = 150);

// "NN - Title" when the title is known, otherwise "TrackNN" (no extension).
std::string trackFileBaseName(const TrackMetadata& track);

// "Artist - Album" (or just the album title) when known, otherwise "cd_<disc id>".
std::string albumDirectoryName(const AlbumMetadata& album);

// Base name (without extension) for files that cover the whole album, such as
// a single-file image and its CUE sheet: "Artist - Album" (or just the album
// title) when known, otherwise `fallback`.
std::string albumFileBase(const AlbumMetadata& album, const std::string& fallback);

}  // namespace cdr
