#include "cdreader/cue_sheet.h"

#include <cctype>
#include <cstdio>
#include <stdexcept>

#include "cdreader/subchannel.h"

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
    c.isrc = m.isrc;
    return c;
}

}  // namespace

std::vector<CueTrack> singleFileCueTracks(const std::vector<Track>& tracks, const std::string& file,
                                          const AlbumMetadata& album, const DiscGaps& gaps, bool withHtoa) {
    std::vector<CueTrack> out;
    uint32_t position = 0;
    if (withHtoa && !tracks.empty()) {
        if (tracks.front().number != 1 || gaps.htoaSectors == 0 || tracks.front().startLba != gaps.htoaSectors)
            throw std::invalid_argument("the image can only start with an HTOA before track 1");
        position = gaps.htoaSectors;
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& t = tracks[i];
        if (i > 0 && (t.number != tracks[i - 1].number + 1 || t.startLba != tracks[i - 1].endLba()))
            throw std::invalid_argument("tracks " + std::to_string(tracks[i - 1].number) + " and " +
                                        std::to_string(t.number) + " are not adjacent on the disc");
        CueTrack c = makeCueTrack(t, file, position, album);
        // The pregap is inside the image when it lies within what precedes
        // the track in the file: the previous track or the HTOA.
        const uint32_t pregap = gaps.pregap(t.number);
        if (pregap > 0 && pregap <= position && (i > 0 || (withHtoa && t.number == 1))) {
            c.hasIndex00 = true;
            c.index00Sectors = position - pregap;
        } else if (i == 0 && t.number == 1 && gaps.htoaSectors > 0 && !withHtoa) {
            c.pregapCommandSectors = gaps.htoaSectors;
        }
        for (uint32_t lba : gaps.laterIndexes(t.number))
            if (lba > t.startLba && lba < t.endLba()) c.laterIndexes.push_back(position + (lba - t.startLba));
        out.push_back(c);
        position += t.lengthSectors;
    }
    return out;
}

std::vector<CueTrack> perTrackCueTracks(const std::vector<Track>& tracks, const std::vector<std::string>& files,
                                        const AlbumMetadata& album, const DiscGaps& gaps,
                                        const std::string& htoaFile) {
    if (files.size() != tracks.size()) throw std::invalid_argument("one file per track expected");
    std::vector<CueTrack> out;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& t = tracks[i];
        CueTrack c = makeCueTrack(t, files[i], 0, album);
        const uint32_t pregap = gaps.pregap(t.number);
        if (t.number == 1 && gaps.htoaSectors > 0) {
            if (!htoaFile.empty()) {
                c.hasIndex00 = true;
                c.index00File = htoaFile;
                c.index00Sectors = 0;
            } else {
                c.pregapCommandSectors = gaps.htoaSectors;
            }
        } else if (pregap > 0 && i > 0) {
            const Track& prev = tracks[i - 1];
            if (prev.number + 1 == t.number && prev.endLba() == t.startLba && pregap <= prev.lengthSectors) {
                c.hasIndex00 = true;
                c.index00File = files[i - 1];
                c.index00Sectors = prev.lengthSectors - pregap;
            }
        }
        for (uint32_t lba : gaps.laterIndexes(t.number))
            if (lba > t.startLba && lba < t.endLba()) c.laterIndexes.push_back(lba - t.startLba);
        out.push_back(c);
    }
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
    if (isValidMcn(album.mcn)) line("CATALOG " + album.mcn);
    if (!album.artist.empty()) line("PERFORMER " + quoted(album.artist));
    if (!album.title.empty()) line("TITLE " + quoted(album.title));

    std::string currentFile;
    bool first = true;
    auto fileLine = [&](const std::string& file) {
        if (!first && file == currentFile) return;
        const size_t dot = file.rfind('.');
        line("FILE " + quoted(file) + " " + cueFileType(dot == std::string::npos ? "" : file.substr(dot + 1)));
        currentFile = file;
        first = false;
    };
    for (const CueTrack& t : tracks) {
        // A track whose INDEX 00 is in another file (the previous track's)
        // starts in that file; its own FILE line comes before INDEX 01.
        fileLine(t.hasIndex00 && !t.index00File.empty() ? t.index00File : t.file);
        char head[32];
        std::snprintf(head, sizeof head, "  TRACK %02d AUDIO", t.number);
        line(head);
        if (!t.title.empty()) line("    TITLE " + quoted(t.title));
        if (!t.performer.empty()) line("    PERFORMER " + quoted(t.performer));
        if (t.preEmphasis || t.copyPermitted)
            line(std::string("    FLAGS") + (t.copyPermitted ? " DCP" : "") + (t.preEmphasis ? " PRE" : ""));
        if (isValidIsrc(t.isrc)) line("    ISRC " + t.isrc);
        if (t.pregapCommandSectors) line("    PREGAP " + formatCueTime(t.pregapCommandSectors));
        if (t.hasIndex00) line("    INDEX 00 " + formatCueTime(t.index00Sectors));
        fileLine(t.file);
        line("    INDEX 01 " + formatCueTime(t.startSectors));
        for (size_t i = 0; i < t.laterIndexes.size() && i < 98; ++i) {
            std::snprintf(head, sizeof head, "    INDEX %02d ", int(i + 2));
            line(head + formatCueTime(t.laterIndexes[i]));
        }
    }

    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) return "\xEF\xBB\xBF" + s;
    return s;
}

}  // namespace cdr
