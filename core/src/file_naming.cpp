#include "cdreader/file_naming.h"

#include <cstdio>
#include <cstring>

namespace cdr {

namespace {

bool isReservedDeviceName(const std::string& name) {
    // Windows treats "CON.txt" like "CON", so compare the part before the first dot.
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (char& c : base) c = char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    static const char* fixed[] = {"CON", "PRN", "AUX", "NUL"};
    for (const char* r : fixed)
        if (base == r) return true;
    return base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) &&
           base[3] >= '1' && base[3] <= '9';
}

void trimEnd(std::string& s) {
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
}

}  // namespace

std::string sanitizeFileName(const std::string& name, size_t maxBytes) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out += (c < 0x20 || c == 0x7F || std::strchr("<>:\"/\\|?*", ch)) ? '_' : ch;
    }
    const size_t first = out.find_first_not_of(' ');
    out.erase(0, first == std::string::npos ? out.size() : first);

    if (out.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    trimEnd(out);
    if (isReservedDeviceName(out)) out.insert(0, "_");
    return out;
}

std::string trackFileBaseName(const TrackMetadata& track) {
    char number[16];
    std::snprintf(number, sizeof number, "%02d", track.trackNumber);
    const std::string title = sanitizeFileName(track.title);
    if (title.empty()) return std::string("Track") + number;
    return std::string(number) + " - " + title;
}

// "Artist - Album", the album title alone, or "" when the title is unknown.
static std::string albumName(const AlbumMetadata& album) {
    const std::string artist = sanitizeFileName(album.artist);
    const std::string title = sanitizeFileName(album.title);
    if (!artist.empty() && !title.empty() && artist != title) return sanitizeFileName(album.artist + " - " + album.title);
    return title;
}

std::string albumDirectoryName(const AlbumMetadata& album) {
    const std::string name = albumName(album);
    return name.empty() ? "cd_" + album.discId : name;
}

std::string albumFileBase(const AlbumMetadata& album, const std::string& fallback) {
    const std::string name = albumName(album);
    return name.empty() ? fallback : name;
}

}  // namespace cdr
