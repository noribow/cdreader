// cdreader - command line CD audio ripper for Windows.

#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/crc32.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/file_naming.h"
#include "cdreader/metadata.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"
#include "spti_transport.h"
#include "winhttp_client.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.1.0";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string formatList() {
    std::string list;
    for (const std::string& f : cdr::audioFormats()) list += (list.empty() ? "" : ", ") + f;
    return list;
}

void printUsage() {
    std::printf(
        "cdreader %s - CD audio ripper\n"
        "\n"
        "Usage:\n"
        "  cdreader drives                       List optical drives\n"
        "  cdreader toc <drive> [cddb options]   Show the table of contents and disc info\n"
        "  cdreader rip <drive> [options]        Rip audio tracks (see --format)\n"
        "  cdreader offset <drive> [options]     Detect the drive read offset (AccurateRip)\n"
        "\n"
        "Rip options:\n"
        "  -o, --output <dir>    Output directory (default: \"Artist - Album\" from CDDB,\n"
        "                        otherwise cd_<CDDB id>)\n"
        "                        A CUE sheet (<album>.cue) is written next to the audio\n"
        "  -f, --format <name>   Output format: %s (default: wav)\n"
        "                        opus = Ogg Opus (.opus), vorbis = Ogg Vorbis (.ogg)\n"
        "                        (when built with libopus / libvorbis)\n"
        "                        mka = Matroska (.mka) with FLAC; mka-pcm, mka-opus,\n"
        "                        mka-vorbis = Matroska with that codec\n"
        "  -b, --bitrate <kbps>  Lossy formats: target bitrate in kbit/s (VBR)\n"
        "                        opus, mka-opus: 6-510 (default 160),\n"
        "                        vorbis, mka-vorbis: 45-500 (average)\n"
        "  -q, --quality <q>     vorbis, mka-vorbis: VBR quality -1..10 (default 5,\n"
        "                        about 160 kbit/s)\n"
        "  -t, --tracks <list>   Tracks to rip, e.g. 1,3-5 (default: all audio tracks)\n"
        "  -r, --retries <n>     Retries per failing read (default: 5)\n"
        "      --offset <n>      Drive read offset correction in samples, e.g. 6 or -472\n"
        "                        (same value as EAC / AccurateRip; default: 0)\n"
        "      --verify          Read everything twice and compare (slower)\n"
        "      --no-accuraterip  Do not look up the AccurateRip database after ripping\n"
        "      --single-file     Rip the tracks into one file (an image of the disc);\n"
        "                        the CUE sheet then marks the track positions. FLAC\n"
        "                        images also carry the CUE sheet inside the file,\n"
        "                        Matroska images a chapter per track\n"
        "      --no-cue-file     Do not write the external .cue file\n"
        "\n"
        "Offset options:\n"
        "  -t, --track <n>       Track to compare (default: the best known track)\n"
        "      --range <n>       Offsets to try, -n..+n samples (default: 3000)\n"
        "  -r, --retries <n>     Retries per failing read (default: 5)\n"
        "\n"
        "CDDB options (rip and toc):\n"
        "      --no-cddb         Do not look up the disc online\n"
        "      --cddb-server <url>  CDDB HTTP server (default: %s)\n"
        "      --cddb-match <n>  Use the n-th match when there are several (default: 1)\n"
        "      --cddb-hello <user@host>  User and host sent in the CDDB greeting\n"
        "                        (default: cdreader@localhost)\n"
        "\n"
        "<drive> is a drive letter such as D or D:\n",
        kVersion, formatList().c_str(), cdr::kDefaultCddbServer);
}

std::vector<std::string> utf8Arguments() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), len, nullptr, nullptr);
        args.push_back(s);
    }
    LocalFree(argv);
    return args;
}

char parseDriveLetter(const std::string& arg) {
    if ((arg.size() == 1 || (arg.size() == 2 && arg[1] == ':')) && std::isalpha(static_cast<unsigned char>(arg[0])))
        return char(std::toupper(static_cast<unsigned char>(arg[0])));
    throw UsageError("invalid drive '" + arg + "' (expected a letter such as D or D:)");
}

int parseSignedInt(const std::string& s, const char* what) {
    try {
        size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size() && !s.empty() && s[0] != ' ') return v;
    } catch (const std::exception&) {
    }
    throw UsageError(std::string("invalid ") + what + " '" + s + "'");
}

double parseNumber(const std::string& s, const char* what) {
    try {
        size_t used = 0;
        const double v = std::stod(s, &used);
        if (used == s.size() && !s.empty() && s[0] != ' ' && std::isfinite(v)) return v;
    } catch (const std::exception&) {
    }
    throw UsageError(std::string("invalid ") + what + " '" + s + "'");
}

int parseInt(const std::string& s, const char* what) {
    const int v = parseSignedInt(s, what);
    if (v < 0) throw UsageError(std::string("invalid ") + what + " '" + s + "'");
    return v;
}

std::set<int> parseTrackList(const std::string& spec) {
    std::set<int> tracks;
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t dash = item.find('-');
        int from = parseInt(item.substr(0, dash), "track number");
        int to = dash == std::string::npos ? from : parseInt(item.substr(dash + 1), "track number");
        if (from < 1 || to > 99 || from > to) throw UsageError("invalid track range '" + item + "'");
        for (int t = from; t <= to; ++t) tracks.insert(t);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return tracks;
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

struct CddbSettings {
    bool enabled = true;
    cdr::CddbOptions options;
};

// Consumes a CDDB option at args[i]; returns false if args[i] is not one.
bool parseCddbOption(const std::vector<std::string>& args, size_t& i, CddbSettings& cddb) {
    const std::string& a = args[i];
    auto value = [&]() -> const std::string& {
        if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
        return args[++i];
    };
    if (a == "--no-cddb") {
        cddb.enabled = false;
    } else if (a == "--cddb-server") {
        cddb.options.server = value();
        if (cddb.options.server.rfind("http://", 0) != 0 && cddb.options.server.rfind("https://", 0) != 0)
            throw UsageError("CDDB server must be an http:// or https:// URL");
    } else if (a == "--cddb-hello") {
        // Some servers want to see a contact address in the greeting.
        const std::string& hello = value();
        const size_t at = hello.find('@');
        if (at == 0 || at == std::string::npos || at + 1 == hello.size())
            throw UsageError("--cddb-hello expects user@host");
        cddb.options.client.user = hello.substr(0, at);
        cddb.options.client.host = hello.substr(at + 1);
    } else if (a == "--cddb-match") {
        const int n = parseInt(value(), "CDDB match number");
        if (n < 1) throw UsageError("CDDB match numbers start at 1");
        cddb.options.matchIndex = size_t(n - 1);
    } else {
        return false;
    }
    return true;
}

// Looks the disc up and reports progress on stdout. Never throws.
cdr::CddbLookupResult lookupDisc(const cdr::Toc& toc, const CddbSettings& cddb) {
    cdr::CddbLookupResult result;
    if (!cddb.enabled) {
        result.error = "disabled (--no-cddb)";
        return result;
    }
    std::printf("Looking up the disc on %s ...\n", cddb.options.server.c_str());
    std::fflush(stdout);
    try {
        cdr::win::WinHttpClient http(std::string("cdreader/") + kVersion);
        cdr::CddbOptions options = cddb.options;
        options.client.version = kVersion;
        result = cdr::lookupCddb(http, toc, options);
    } catch (const std::exception& e) {
        result = {};
        result.error = e.what();
    }
    if (!result.found) {
        std::printf("CDDB: %s\n\n", result.error.c_str());
        return result;
    }
    if (result.matches.size() > 1 || !result.exact) {
        std::printf("CDDB: %d %s match(es):\n", int(result.matches.size()), result.exact ? "exact" : "inexact");
        for (size_t i = 0; i < result.matches.size(); ++i) {
            const cdr::CddbMatch& m = result.matches[i];
            std::printf("  %c%d. %s/%s  %s\n", i == result.chosen ? '*' : ' ', int(i + 1), m.category.c_str(),
                        m.discId.c_str(), m.title.c_str());
        }
        if (cddb.options.matchIndex >= result.matches.size())
            std::printf("  (--cddb-match %d is out of range, using match 1)\n", int(cddb.options.matchIndex + 1));
        else if (result.matches.size() > 1)
            std::printf("  (use --cddb-match <n> to pick another one)\n");
    }
    return result;
}

void printAlbum(const cdr::AlbumMetadata& album, FILE* out) {
    std::fprintf(out, "Artist: %s\nAlbum:  %s\n", album.artist.c_str(), album.title.c_str());
    if (!album.year.empty()) std::fprintf(out, "Year:   %s\n", album.year.c_str());
    if (!album.genre.empty()) std::fprintf(out, "Genre:  %s\n", album.genre.c_str());
}

// "Title" or "Artist / Title" of a track, empty if unknown.
std::string trackLabel(const cdr::AlbumMetadata& album, int number, int total) {
    const cdr::TrackMetadata m = album.forTrack(number, total);
    if (m.title.empty()) return {};
    return m.artist != album.artist && !m.artist.empty() ? m.artist + " / " + m.title : m.title;
}

struct OpenedDrive {
    std::unique_ptr<cdr::win::SptiTransport> transport;
    std::unique_ptr<cdr::CdDrive> drive;
    cdr::DriveInfo info;
};

OpenedDrive openDrive(char letter) {
    OpenedDrive d;
    d.transport = cdr::win::SptiTransport::open(letter);
    d.drive = std::make_unique<cdr::CdDrive>(*d.transport);
    d.info = d.drive->inquiry();
    return d;
}

cdr::Toc readTocOrExplain(cdr::CdDrive& drive) {
    if (!drive.isReady()) throw std::runtime_error("drive is not ready (no disc inserted?)");
    return drive.readToc();
}

void printToc(const cdr::Toc& toc, const cdr::AlbumMetadata& album, FILE* out) {
    std::fprintf(out, "CDDB disc id: %s\n", hex32(toc.cddbId()).c_str());
    std::fprintf(out, "AccurateRip disc id: %s\n", cdr::AccurateRipDiscId::fromToc(toc).toString().c_str());
    std::fprintf(out, "Track  Start LBA   Length     Type\n");
    for (const cdr::Track& t : toc.tracks) {
        const std::string title = trackLabel(album, t.number, toc.lastTrack);
        std::fprintf(out, "  %2d   %9u   %s   %-5s%s%s%s\n", t.number, t.startLba,
                     cdr::formatMsf(t.lengthSectors).c_str(), t.isAudio ? "audio" : "data",
                     t.preEmphasis ? " (pre-emphasis)" : "", title.empty() ? "" : "  ", title.c_str());
    }
    std::fprintf(out, "Lead-out at LBA %u, total %s\n", toc.leadOutLba, cdr::formatMsf(toc.leadOutLba).c_str());
}

int cmdDrives() {
    const std::vector<char> letters = cdr::win::listOpticalDrives();
    if (letters.empty()) {
        std::printf("No optical drives found.\n");
        return 1;
    }
    for (char letter : letters) {
        try {
            OpenedDrive d = openDrive(letter);
            std::printf("%c:  %s\n", letter, d.info.displayName().c_str());
        } catch (const std::exception& e) {
            std::printf("%c:  (%s)\n", letter, e.what());
        }
    }
    return 0;
}

int cmdToc(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("toc needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    CddbSettings cddb;
    for (size_t i = 2; i < args.size(); ++i)
        if (!parseCddbOption(args, i, cddb)) throw UsageError("unknown option '" + args[i] + "'");

    OpenedDrive d = openDrive(letter);
    std::printf("Drive: %s\n", d.info.displayName().c_str());
    const cdr::Toc toc = readTocOrExplain(*d.drive);
    const cdr::CddbLookupResult found = lookupDisc(toc, cddb);
    if (found.found) {
        printAlbum(found.album, stdout);
        std::printf("\n");
    }
    printToc(toc, found.album, stdout);
    return 0;
}

struct ArTrack {
    cdr::Track track;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
};

// Looks up the rip in the AccurateRip database and prints the per-track
// verdicts to the console and the log. Failures only produce a message.
void reportAccurateRip(const cdr::Toc& toc, const std::vector<ArTrack>& tracks, bool lookup, char letter,
                       std::ostream& log) {
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(toc);
    auto line = [&](const std::string& text) {
        std::printf("%s\n", text.c_str());
        log << text << "\n";
    };
    std::printf("\n");
    log << "\n";

    cdr::AccurateRipLookup result;
    if (lookup) {
        std::printf("Looking up AccurateRip database...\r");
        std::fflush(stdout);
        cdr::win::WinHttpClient http(std::string("cdreader/") + kVersion);
        result = cdr::lookupAccurateRip(http, id);
    }
    const bool found = result.status == cdr::AccurateRipLookup::Status::Found;

    line("AccurateRip (disc id " + id.toString() + ")" +
         (found ? ", " + std::to_string(result.pressings.size()) + " pressing(s) in database" : std::string()));
    int accurate = 0;
    int inDatabase = 0;
    std::map<std::string, int> byVersion;  // accurate tracks per matched checksum version
    for (const ArTrack& t : tracks) {
        const cdr::AccurateRipTrackResult r =
            cdr::matchAccurateRip(result.pressings, cdr::accurateRipEntryIndex(toc, t.track), t.track.number, t.v1, t.v2);
        char buf[96];
        std::snprintf(buf, sizeof buf, "Track %02d  v1 %s  v2 %s  ", t.track.number, hex32(t.v1).c_str(),
                      hex32(t.v2).c_str());
        line(buf + (found ? r.describe() : std::string()));
        // Per pressing: its checksum, how many submissions stand behind it and
        // which of our checksums (if any) it equals.
        for (const cdr::AccurateRipPressingMatch& p : r.pressings) {
            std::snprintf(buf, sizeof buf, "          pressing %d: %s  confidence %3d  %s", p.pressing,
                          hex32(p.checksum).c_str(), p.confidence,
                          p.version == 2 ? "v2 match" : p.version == 1 ? "v1 match" : "no match");
            line(buf);
        }
        accurate += r.accurate() ? 1 : 0;
        inDatabase += r.inDatabase() ? 1 : 0;
        if (r.accurate()) ++byVersion[r.matchedVersion()];
    }

    if (!lookup) {
        line("AccurateRip: lookup disabled (--no-accuraterip)");
    } else if (result.status == cdr::AccurateRipLookup::Status::NotFound) {
        line("AccurateRip: this disc is not in the database (no rips submitted yet)");
    } else if (!found) {
        line("AccurateRip: lookup failed: " + result.error);
    } else {
        std::string versions;
        for (const auto& [version, count] : byVersion)
            versions += (versions.empty() ? "" : ", ") + version + ": " + std::to_string(count);
        line("AccurateRip: " + std::to_string(accurate) + " of " + std::to_string(tracks.size()) +
             " track(s) accurately ripped" + (versions.empty() ? "" : " (" + versions + ")") + ", " +
             std::to_string(inDatabase) + " track(s) in database");
        if (accurate == 0 && inDatabase > 0)
            line(std::string("Hint: no track matched. Check the read offset (--offset), e.g. with 'cdreader offset ") +
                 letter + ":'.");
    }
}

int cmdRip(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("rip needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    std::string outputDir;
    std::string format = "wav";
    std::set<int> wanted;
    cdr::RipOptions options;
    CddbSettings cddb;
    bool accurateRip = true;
    bool singleFile = false;
    bool cueFile = true;
    cdr::EncoderSettings encoder;

    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
            return args[++i];
        };
        if (a == "-o" || a == "--output") outputDir = value();
        else if (a == "-f" || a == "--format") format = value();
        else if (a == "-b" || a == "--bitrate") encoder.bitrateKbps = parseSignedInt(value(), "bitrate");
        else if (a == "-q" || a == "--quality") encoder.quality = parseNumber(value(), "quality");
        else if (a == "-t" || a == "--tracks") wanted = parseTrackList(value());
        else if (a == "-r" || a == "--retries") options.maxRetries = parseInt(value(), "retry count");
        else if (a == "--offset") options.readOffsetSamples = parseSignedInt(value(), "read offset");
        else if (a == "--no-cue-file") cueFile = false;
        else if (a == "--verify") options.verify = true;
        else if (parseCddbOption(args, i, cddb)) continue;
        else if (a == "--no-accuraterip") accurateRip = false;
        else if (a == "--single-file") singleFile = true;
        else throw UsageError("unknown option '" + a + "'");
    }

    std::unique_ptr<cdr::AudioWriter> probe;
    try {
        probe = cdr::createAudioWriter(format, encoder);
    } catch (const std::invalid_argument& e) {
        throw UsageError(std::string(e.what()) +
                         (cdr::isLossyFormat(format) ? "" : " (--bitrate / --quality are for lossy formats)"));
    }
    if (!probe) {
        if (cdr::isLossyFormat(format))
            throw UsageError("format '" + format + "' is not available in this build (available: " + formatList() + ")");
        throw UsageError("unknown format '" + format + "' (available: " + formatList() + ")");
    }

    OpenedDrive d = openDrive(letter);
    const cdr::Toc toc = readTocOrExplain(*d.drive);

    std::vector<cdr::Track> selected;
    for (const cdr::Track& t : toc.tracks) {
        if (!wanted.empty() && !wanted.count(t.number)) continue;
        if (!t.isAudio) {
            if (!wanted.empty()) std::printf("Skipping track %d: data track\n", t.number);
            continue;
        }
        selected.push_back(t);
    }
    for (int n : wanted)
        if (!toc.findTrack(n)) throw UsageError("track " + std::to_string(n) + " is not on this disc");
    if (selected.empty()) throw std::runtime_error("no audio tracks to rip");
    // Far beyond any real drive (known offsets are within about +-3000 samples).
    if (std::abs(options.readOffsetSamples) > int(100 * cdr::kSamplesPerSector))
        throw UsageError("read offset out of range");

    std::printf("Drive: %s\n", d.info.displayName().c_str());

    // Metadata is optional: a failed lookup only means generic names.
    const cdr::CddbLookupResult found = lookupDisc(toc, cddb);
    cdr::AlbumMetadata album;
    if (found.found) {
        album = found.album;
        printAlbum(album, stdout);
    }
    album.discId = hex32(toc.cddbId());

    // In single-file mode the tracks are written back to back, which is only
    // an image of the disc if they are adjacent on it.
    const std::string extension = probe->extension();
    const std::string encoderText = probe->encoderDescription();
    const std::string albumBase = cdr::albumFileBase(album, "CDImage");
    const std::string imageName = albumBase + "." + extension;
    std::vector<cdr::CueTrack> cueTracks;
    if (singleFile) {
        try {
            cueTracks = cdr::singleFileCueTracks(selected, imageName, album);
        } catch (const std::invalid_argument& e) {
            throw UsageError(std::string("--single-file needs consecutive audio tracks (") + e.what() + ")");
        }
    }

    const fs::path dir = fs::u8path(outputDir.empty() ? cdr::albumDirectoryName(album) : outputDir);
    fs::create_directories(dir);

    std::printf("Read offset correction: %+d samples\n", options.readOffsetSamples);
    std::printf("Format: %s\n", format.c_str());
    if (!encoderText.empty()) std::printf("Encoder: %s\n", encoderText.c_str());
    std::printf("Output: %s\n\n", dir.u8string().c_str());

    std::ofstream log(dir / "rip.log");
    log << "cdreader " << kVersion << " rip log\n"
        << "Drive: " << d.info.displayName() << "\n"
        << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n"
        << "Read offset correction: " << (options.readOffsetSamples > 0 ? "+" : "") << options.readOffsetSamples
        << " samples\n"
        << "Format: " << format << "\n";
    if (!encoderText.empty()) log << "Encoder: " << encoderText << "\n";
    log << "\n";
    {
        char line[256];
        log << "CDDB disc id: " << hex32(toc.cddbId()) << "\n";
        log << "AccurateRip disc id: " << cdr::AccurateRipDiscId::fromToc(toc).toString() << "\n";
        for (const cdr::Track& t : toc.tracks) {
            std::snprintf(line, sizeof line, "Track %2d  LBA %7u  %s  %-5s", t.number, t.startLba,
                          cdr::formatMsf(t.lengthSectors).c_str(), t.isAudio ? "audio" : "data");
            const std::string title = trackLabel(album, t.number, toc.lastTrack);
            log << line << (title.empty() ? "" : "  ") << title << "\n";
        }
        log << "\n";
    }
    if (!cddb.enabled) {
        log << "CDDB lookup: disabled\n\n";
    } else if (!found.found) {
        log << "CDDB lookup (" << cddb.options.server << "): " << found.error << "\n\n";
    } else {
        log << "CDDB lookup (" << cddb.options.server << "): " << found.matches.size()
            << (found.exact ? " exact" : " inexact") << " match(es)\n";
        for (size_t i = 0; i < found.matches.size(); ++i) {
            const cdr::CddbMatch& m = found.matches[i];
            log << (i == found.chosen ? "  * " : "    ") << i + 1 << ". " << m.category << "/" << m.discId << "  "
                << m.title << "\n";
        }
        log << "Artist: " << album.artist << "\nAlbum: " << album.title << "\nYear: " << album.year
            << "\nGenre: " << album.genre << "\n\n";
    }


    cdr::Ripper ripper(*d.drive, toc, options);
    int problems = 0;
    std::vector<ArTrack> arTracks;
    std::unique_ptr<cdr::AudioWriter> writer;
    cdr::Crc32 imageCrc;
    std::vector<std::string> trackFiles;
    if (singleFile) {
        writer = cdr::createAudioWriter(format, encoder);
        if (writer->canEmbedCueSheet()) {
            cdr::EmbeddedCueSheet embedded;
            embedded.tracks = cueTracks;
            for (const cdr::Track& t : selected) embedded.totalSectors += t.lengthSectors;
            embedded.text = cdr::formatCueSheet(album, cueTracks);
            writer->setEmbeddedCueSheet(embedded);
        }
        writer->open(dir / fs::u8path(imageName), album.forTrack(0, toc.lastTrack));
        log << "Single file: " << imageName << "\n";
        if (writer->canEmbedCueSheet()) {
            // FLAC: CUESHEET block + tag; Matroska: chapters (#23).
            const char* what = extension == "mka" ? "Matroska chapters (one per track)" : "CUESHEET block and tag";
            std::printf("Embedded CUE sheet: %s in %s\n\n", what, imageName.c_str());
            log << "Embedded CUE sheet: " << what << "\n";
        }
    }
    for (const cdr::Track& t : selected) {
        std::string name;
        if (singleFile) {
            char base[32];
            std::snprintf(base, sizeof base, "Track %02d", t.number);
            name = base;
        } else {
            writer = cdr::createAudioWriter(format, encoder);
            const cdr::TrackMetadata metadata = album.forTrack(t.number, toc.lastTrack);
            name = cdr::trackFileBaseName(metadata) + "." + extension;
            writer->open(dir / fs::u8path(name), metadata);
            trackFiles.push_back(name);
        }
        int lastPercent = -1;
        cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, t);
        cdr::TrackRipResult r = ripper.ripTrack(
            t,
            [&](const uint8_t* pcm, size_t bytes) {
                writer->write(pcm, bytes);
                ar.update(pcm, bytes);
                if (singleFile) imageCrc.update(pcm, bytes);
            },
            [&](uint32_t done, uint32_t total) {
                int percent = total ? int(uint64_t(done) * 100 / total) : 100;
                if (percent != lastPercent) {
                    std::printf("\rTrack %02d [%s]  %3d%%", t.number, cdr::formatMsf(t.lengthSectors).c_str(), percent);
                    std::fflush(stdout);
                    lastPercent = percent;
                }
            });
        if (!singleFile) writer->close();

        const std::string status = r.clean() ? "OK" : std::to_string(r.unreadableSectors) + " unreadable sector(s)";
        std::printf("  CRC32 %s  %s%s\n", hex32(r.crc32).c_str(), status.c_str(),
                    r.retries ? ("  (" + std::to_string(r.retries) + " retries)").c_str() : "");
        log << name << "  CRC32 " << hex32(r.crc32) << "  retries " << r.retries << "  " << status;
        if (r.paddedSamples) log << "  (" << r.paddedSamples << " samples outside the disc padded with silence)";
        log << "\n";
        if (!r.clean()) ++problems;
        arTracks.push_back({t, ar.v1(), ar.v2()});
    }
    if (singleFile) {
        writer->close();
        log << imageName << "  CRC32 " << hex32(imageCrc.value()) << "\n";
    }

    if (cueFile) {
        const std::string cueName = albumBase + ".cue";
        const std::string cue =
            cdr::formatCueSheet(album, singleFile ? cueTracks : cdr::perTrackCueTracks(selected, trackFiles, album));
        std::ofstream out(dir / fs::u8path(cueName), std::ios::binary);
        out << cue;
        if (!out.flush()) throw std::runtime_error("failed to write " + cueName);
        std::printf("CUE sheet: %s\n", cueName.c_str());
    }

    reportAccurateRip(toc, arTracks, accurateRip, letter, log);

    log << "\n" << (problems ? "Finished with errors" : "All tracks ripped without errors") << "\n";
    std::printf("\n%s\n", problems ? "Finished with errors - see rip.log" : "Done.");
    return problems ? 2 : 0;
}

// Detects the read offset by reading one track and comparing its checksum
// at every candidate offset with the AccurateRip database.
int cmdOffset(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("offset needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    int trackNumber = 0;
    int range = 3000;
    cdr::RipOptions options;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
            return args[++i];
        };
        if (a == "-t" || a == "--track") trackNumber = parseInt(value(), "track number");
        else if (a == "--range") range = parseInt(value(), "offset range");
        else if (a == "-r" || a == "--retries") options.maxRetries = parseInt(value(), "retry count");
        else throw UsageError("unknown option '" + a + "'");
    }
    if (range < 1 || range > int(100 * cdr::kSamplesPerSector)) throw UsageError("offset range out of range");

    OpenedDrive d = openDrive(letter);
    const cdr::Toc toc = readTocOrExplain(*d.drive);
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(toc);
    std::printf("Drive: %s\n", d.info.displayName().c_str());
    std::printf("AccurateRip disc id: %s\n", id.toString().c_str());

    cdr::win::WinHttpClient http(std::string("cdreader/") + kVersion);
    const cdr::AccurateRipLookup lookup = cdr::lookupAccurateRip(http, id);
    if (lookup.status == cdr::AccurateRipLookup::Status::NotFound) {
        std::printf("This disc is not in the AccurateRip database. Try a more popular CD.\n");
        return 1;
    }
    if (lookup.status != cdr::AccurateRipLookup::Status::Found)
        throw std::runtime_error("AccurateRip lookup failed: " + lookup.error);

    // Total confidence of a track's database entries.
    auto confidence = [&](const cdr::Track& t) {
        return cdr::matchAccurateRip(lookup.pressings, cdr::accurateRipEntryIndex(toc, t), t.number, 0, 0)
            .totalConfidence;
    };
    const cdr::Track* track = nullptr;
    if (trackNumber) {
        track = toc.findTrack(trackNumber);
        if (!track || !track->isAudio) throw UsageError("track " + std::to_string(trackNumber) + " is not an audio track");
    } else {
        // Prefer tracks in the middle of the disc: the first and last ones
        // touch the unreadable area outside the disc at large offsets.
        const size_t audio = toc.audioTrackCount();
        size_t index = 0;
        int best = 0;
        for (const cdr::Track& t : toc.tracks) {
            if (!t.isAudio) continue;
            ++index;
            const bool edge = audio > 2 && (index == 1 || index == audio);
            const int c = confidence(t) * (edge ? 1 : 1000);
            if (c > best) {
                best = c;
                track = &t;
            }
        }
        if (!track) throw std::runtime_error("no track of this disc has AccurateRip data");
    }

    std::printf("Checking track %d (confidence %d) at offsets -%d..+%d\n", track->number, confidence(*track), range,
                range);
    int lastPercent = -1;
    const cdr::AccurateRipOffsetScan scan = cdr::scanReadOffsets(
        *d.drive, toc, *track, uint32_t(range), options, [&](uint32_t done, uint32_t total) {
            const int percent = total ? int(uint64_t(done) * 100 / total) : 100;
            if (percent != lastPercent) {
                std::printf("\rReading track %02d  %3d%%", track->number, percent);
                std::fflush(stdout);
                lastPercent = percent;
            }
        });
    std::printf("\n");

    const std::vector<cdr::AccurateRipOffsetMatch> matches =
        cdr::findAccurateRipOffsets(scan, lookup.pressings, cdr::accurateRipEntryIndex(toc, *track));
    if (matches.empty()) {
        std::printf("No offset in -%d..+%d matches the database (try another track with -t, or a wider --range).\n",
                    range, range);
        return 1;
    }
    std::printf("Matching offsets (submissions whose checksum matches at that offset):\n");
    for (const cdr::AccurateRipOffsetMatch& m : matches)
        std::printf("  offset %+5d  %-5s  v2 %3d  v1 %3d  (%d of %d pressing(s))\n", m.offset,
                    m.matchedVersion().c_str(), m.v2Confidence, m.v1Confidence, m.pressings, int(lookup.pressings.size()));
    const cdr::AccurateRipOffsetMatch& best = matches.front();
    std::printf("\nRead offset: %+d  (matched %s, confidence %d; use: cdreader rip %c: --offset %d)\n", best.offset,
                best.matchedVersion().c_str(), best.confidence(), letter, best.offset);
    if (matches.size() > 1)
        std::printf("Several offsets match (different pressings); confirm the result with another disc.\n");
    return 0;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const std::vector<std::string> args = utf8Arguments();
    try {
        if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
            printUsage();
            return args.empty() ? 1 : 0;
        }
        if (args[0] == "--version") {
            std::printf("cdreader %s\n", kVersion);
            return 0;
        }
        if (args[0] == "drives") return cmdDrives();
        if (args[0] == "toc") return cmdToc(args);
        if (args[0] == "rip") return cmdRip(args);
        if (args[0] == "offset") return cmdOffset(args);
        throw UsageError("unknown command '" + args[0] + "'");
    } catch (const UsageError& e) {
        std::fprintf(stderr, "error: %s\n\n", e.what());
        printUsage();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nerror: %s\n", e.what());
        return 1;
    }
}
