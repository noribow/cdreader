// cdreader - command line CD audio ripper for Windows.

#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
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

void printUsage() {
    std::printf(
        "cdreader %s - CD audio ripper\n"
        "\n"
        "Usage:\n"
        "  cdreader drives                       List optical drives\n"
        "  cdreader toc <drive> [cddb options]   Show the table of contents and disc info\n"
        "  cdreader rip <drive> [options]        Rip audio tracks to WAV\n"
        "\n"
        "Rip options:\n"
        "  -o, --output <dir>    Output directory (default: \"Artist - Album\" from CDDB,\n"
        "                        otherwise cd_<CDDB id>)\n"
        "  -t, --tracks <list>   Tracks to rip, e.g. 1,3-5 (default: all audio tracks)\n"
        "  -r, --retries <n>     Retries per failing read (default: 5)\n"
        "      --offset <n>      Drive read offset correction in samples, e.g. 6 or -472\n"
        "                        (same value as EAC / AccurateRip; default: 0)\n"
        "      --verify          Read everything twice and compare (slower)\n"
        "\n"
        "CDDB options (rip and toc):\n"
        "      --no-cddb         Do not look up the disc online\n"
        "      --cddb-server <url>  CDDB HTTP server (default: %s)\n"
        "      --cddb-match <n>  Use the n-th match when there are several (default: 1)\n"
        "      --cddb-hello <user@host>  User and host sent in the CDDB greeting\n"
        "                        (default: cdreader@localhost)\n"
        "\n"
        "<drive> is a drive letter such as D or D:\n",
        kVersion, cdr::kDefaultCddbServer);
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
        std::printf("CDDB: %zu %s match(es):\n", result.matches.size(), result.exact ? "exact" : "inexact");
        for (size_t i = 0; i < result.matches.size(); ++i) {
            const cdr::CddbMatch& m = result.matches[i];
            std::printf("  %c%zu. %s/%s  %s\n", i == result.chosen ? '*' : ' ', i + 1, m.category.c_str(),
                        m.discId.c_str(), m.title.c_str());
        }
        if (cddb.options.matchIndex >= result.matches.size())
            std::printf("  (--cddb-match %zu is out of range, using match 1)\n", cddb.options.matchIndex + 1);
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

int cmdRip(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("rip needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    std::string outputDir;
    std::set<int> wanted;
    cdr::RipOptions options;
    CddbSettings cddb;

    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
            return args[++i];
        };
        if (a == "-o" || a == "--output") outputDir = value();
        else if (a == "-t" || a == "--tracks") wanted = parseTrackList(value());
        else if (a == "-r" || a == "--retries") options.maxRetries = parseInt(value(), "retry count");
        else if (a == "--offset") options.readOffsetSamples = parseSignedInt(value(), "read offset");
        else if (a == "--verify") options.verify = true;
        else if (parseCddbOption(args, i, cddb)) continue;
        else throw UsageError("unknown option '" + a + "'");
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

    const fs::path dir = fs::u8path(outputDir.empty() ? cdr::albumDirectoryName(album) : outputDir);
    fs::create_directories(dir);

    std::printf("Read offset correction: %+d samples\n", options.readOffsetSamples);
    std::printf("Output: %s\n\n", dir.u8string().c_str());

    std::ofstream log(dir / "rip.log");
    log << "cdreader " << kVersion << " rip log\n"
        << "Drive: " << d.info.displayName() << "\n"
        << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n"
        << "Read offset correction: " << (options.readOffsetSamples > 0 ? "+" : "") << options.readOffsetSamples
        << " samples\n\n";
    {
        char line[256];
        log << "CDDB disc id: " << hex32(toc.cddbId()) << "\n";
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

    const std::string format = "wav";

    cdr::Ripper ripper(*d.drive, toc, options);
    int problems = 0;
    for (const cdr::Track& t : selected) {
        std::unique_ptr<cdr::AudioWriter> writer = cdr::createAudioWriter(format);
        const cdr::TrackMetadata metadata = album.forTrack(t.number, toc.lastTrack);
        const std::string name = cdr::trackFileBaseName(metadata) + "." + writer->extension();
        const fs::path file = dir / fs::u8path(name);

        writer->open(file, metadata);
        int lastPercent = -1;
        cdr::TrackRipResult r = ripper.ripTrack(
            t, [&](const uint8_t* pcm, size_t bytes) { writer->write(pcm, bytes); },
            [&](uint32_t done, uint32_t total) {
                int percent = total ? int(uint64_t(done) * 100 / total) : 100;
                if (percent != lastPercent) {
                    std::printf("\rTrack %02d [%s]  %3d%%", t.number, cdr::formatMsf(t.lengthSectors).c_str(), percent);
                    std::fflush(stdout);
                    lastPercent = percent;
                }
            });
        writer->close();

        const std::string status = r.clean() ? "OK" : std::to_string(r.unreadableSectors) + " unreadable sector(s)";
        std::printf("  CRC32 %s  %s%s\n", hex32(r.crc32).c_str(), status.c_str(),
                    r.retries ? ("  (" + std::to_string(r.retries) + " retries)").c_str() : "");
        log << name << "  CRC32 " << hex32(r.crc32) << "  retries " << r.retries << "  " << status;
        if (r.paddedSamples) log << "  (" << r.paddedSamples << " samples outside the disc padded with silence)";
        log << "\n";
        if (!r.clean()) ++problems;
    }

    log << "\n" << (problems ? "Finished with errors" : "All tracks ripped without errors") << "\n";
    std::printf("\n%s\n", problems ? "Finished with errors - see rip.log" : "Done.");
    return problems ? 2 : 0;
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
