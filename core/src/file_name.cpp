#include "cdreader/file_name.h"

#include <cctype>
#include <cstring>

namespace cdr {

namespace {

constexpr size_t kMaxNameBytes = 150;

bool isDeviceName(const std::string& name) {
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

}  // namespace

std::string safeFileName(const std::string& text, const std::string& fallback) {
    std::string s;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        s += (u < 0x20 || u == 0x7F || std::strchr("<>:\"/\\|?*", c)) ? '_' : c;
    }
    if (s.size() > kMaxNameBytes) {
        size_t n = kMaxNameBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // don't split a character
        s.resize(n);
    }
    size_t begin = 0;
    while (begin < s.size() && s[begin] == ' ') ++begin;
    s.erase(0, begin);
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s.empty()) return fallback;
    if (isDeviceName(s)) s.insert(0, "_");
    return s;
}

std::string albumFileBase(const AlbumMetadata& album, const std::string& fallback) {
    if (album.title.empty()) return fallback;
    return safeFileName(album.artist.empty() ? album.title : album.artist + " - " + album.title, fallback);
}

}  // namespace cdr
