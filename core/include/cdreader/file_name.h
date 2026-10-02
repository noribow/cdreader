#pragma once

#include <string>

#include "cdreader/metadata.h"

namespace cdr {

// Turns arbitrary UTF-8 text (e.g. an album title) into a file name that is
// valid on Windows and Android: reserved characters and control characters
// become '_', leading/trailing spaces and trailing dots are removed, device
// names such as "CON" are prefixed with '_', and the result is limited to
// 150 bytes (cut on a UTF-8 character boundary). Returns `fallback` when
// nothing usable is left.
std::string safeFileName(const std::string& text, const std::string& fallback);

// Base name (without extension) for files that cover the whole album, such as
// a single-file image and its CUE sheet: "Artist - Title", "Title", or
// `fallback` when the album title is unknown.
std::string albumFileBase(const AlbumMetadata& album, const std::string& fallback);

}  // namespace cdr
