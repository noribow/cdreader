#include "cdreader/cue_sheet.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace cdr {

namespace {

// CUE sheets have no escape mechanism: a '"' would end the string early, so
// it is replaced with '\''. Control characters (line breaks!) become spaces.
std::string quoted(const std::string& s) {
    std::string q = "\"";
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        q += c == '"' ? '\'' : (u < 0x20 || u == 0x7F) ? ' ' : c;
    }
    return q + "\"";
}

// REM values are conventionally unquoted single words (REM DATE 1999) and
// quoted when they contain spaces (REM GENRE "Hard Rock"), as EAC writes them.
std::string remValue(const std::string& s) {
    for (char c : s)
        if (c == ' ' || c == '"' || static_cast<unsigned char>(c) < 0x20) return quoted(s);
    return s;
}

std::string lowercase(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

CueTrack makeCueTrack(const Track& t, const std::string& file, uint32_t start, const AlbumMetadata& album) {
    const TrackMetadata m = album.forTrack(t.number, 0);
    CueTrack c;
    c.number = t.number;
    c.file = file;
    c.startSectors = start;
    c.preEmphasis = t.preEmphasis;
    c.copyPermitted = t.copyPermitted;
    c.title = m.title;
    c.performer = m.artist;
    return c;
}

}  // namespace

std::vector<CueTrack> singleFileCueTracks(const std::vector<Track>& tracks, const std::string& file,
                                          const AlbumMetadata& album) {
    std::vector<CueTrack> out;
    uint32_t position = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i > 0 && (tracks[i].number != tracks[i - 1].number + 1 || tracks[i].startLba != tracks[i - 1].endLba()))
            throw std::invalid_argument("tracks " + std::to_string(tracks[i - 1].number) + " and " +
                                        std::to_string(tracks[i].number) + " are not adjacent on the disc");
        out.push_back(makeCueTrack(tracks[i], file, position, album));
        position += tracks[i].lengthSectors;
    }
    return out;
}

std::vector<CueTrack> perTrackCueTracks(const std::vector<Track>& tracks, const std::vector<std::string>& files,
                                        const AlbumMetadata& album) {
    if (files.size() != tracks.size()) throw std::invalid_argument("one file per track expected");
    std::vector<CueTrack> out;
    for (size_t i = 0; i < tracks.size(); ++i) out.push_back(makeCueTrack(tracks[i], files[i], 0, album));
    return out;
}

std::string cueFileType(const std::string& extension) {
    const std::string e = lowercase(extension);
    if (e == "mp3") return "MP3";
    if (e == "aif" || e == "aiff") return "AIFF";
    return "WAVE";
}

std::string formatCueTime(uint32_t sectors) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02u:%02u:%02u", sectors / (60 * kSectorsPerSecond),
                  (sectors / kSectorsPerSecond) % 60, sectors % kSectorsPerSecond);
    return buf;
}

// Encoding: the CUE format predates Unicode, so readers guess. foobar2000,
// CUETools and current players accept UTF-8 with or without a BOM, but
// Windows software that falls back to the ANSI code page (CP932 on Japanese
// Windows) only recognizes UTF-8 by its BOM. Some older parsers instead treat
// the BOM as part of the first line and ignore that line. So: an ASCII-only
// sheet is written without a BOM (identical in every encoding), a sheet with
// non-ASCII text gets a BOM, and the first line is always a REM comment so a
// parser that drops it loses nothing.
std::string formatCueSheet(const AlbumMetadata& album, const std::vector<CueTrack>& tracks) {
    std::string s;
    auto line = [&](const std::string& text) { s += text + "\r\n"; };

    line("REM COMMENT \"cdreader\"");
    if (!album.genre.empty()) line("REM GENRE " + remValue(album.genre));
    if (!album.year.empty()) line("REM DATE " + remValue(album.year));
    if (!album.discId.empty()) line("REM DISCID " + remValue(album.discId));
    if (!album.artist.empty()) line("PERFORMER " + quoted(album.artist));
    if (!album.title.empty()) line("TITLE " + quoted(album.title));

    std::string currentFile;
    bool first = true;
    for (const CueTrack& t : tracks) {
        if (first || t.file != currentFile) {
            const size_t dot = t.file.rfind('.');
            line("FILE " + quoted(t.file) + " " + cueFileType(dot == std::string::npos ? "" : t.file.substr(dot + 1)));
            currentFile = t.file;
            first = false;
        }
        char head[32];
        std::snprintf(head, sizeof head, "  TRACK %02d AUDIO", t.number);
        line(head);
        if (!t.title.empty()) line("    TITLE " + quoted(t.title));
        if (!t.performer.empty()) line("    PERFORMER " + quoted(t.performer));
        if (t.preEmphasis || t.copyPermitted)
            line(std::string("    FLAGS") + (t.copyPermitted ? " DCP" : "") + (t.preEmphasis ? " PRE" : ""));
        line("    INDEX 01 " + formatCueTime(t.startSectors));
    }

    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return "\xEF\xBB\xBF" + s;
    return s;
}

}  // namespace cdr
