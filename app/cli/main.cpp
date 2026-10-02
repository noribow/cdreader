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
#include <iterator>
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
#include "cdreader/drive_cache.h"
#include "cdreader/file_naming.h"
#include "cdreader/gaps.h"
#include "cdreader/metadata.h"
#include "cdreader/offset_detect.h"
#include "cdreader/ripper.h"
#include "cdreader/subchannel.h"
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

// --- Read offsets saved per drive (#37) ---------------------------------------
// %APPDATA%\cdreader\drive_offsets.txt (the roaming profile of the user);
// next to cdreader.exe when APPDATA is not set. Format: cdr::DriveOffsetStore.
fs::path offsetStorePath() {
    std::wstring appData(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData.data(), DWORD(appData.size()));
    if (n > 0 && n < appData.size()) {
        appData.resize(n);
        return fs::path(appData) / L"cdreader" / L"drive_offsets.txt";
    }
    std::wstring exe(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    exe.resize(len < exe.size() ? len : 0);
    return fs::path(exe).parent_path() / L"drive_offsets.txt";
}

cdr::DriveOffsetStore loadOffsetStore() {
    std::ifstream in(offsetStorePath(), std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return cdr::DriveOffsetStore::parse(text);
}

void saveDriveOffset(const cdr::DriveInfo& info, const cdr::SavedDriveOffset& value) {
    cdr::DriveOffsetStore store = loadOffsetStore();
    store.set(cdr::driveOffsetKey(info), value);
    const fs::path path = offsetStorePath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << store.serialize();
    if (!out.flush()) throw std::runtime_error("cannot write " + path.u8string());
    std::printf("Saved read offset %+d for %s in %s\n", value.offset, info.displayName().c_str(),
                path.u8string().c_str());
}

void printUsage() {
    std::printf(
        "cdreader %s - CD audio ripper\n"
        "\n"
        "Usage:\n"
        "  cdreader drives                       List optical drives (C2 pointer support,\n"
        "                                        cache size)\n"
        "  cdreader toc <drive> [options]        Show the table of contents and disc info\n"
        "                        (CDDB options, --no-isrc and --no-gaps)\n"
        "  cdreader rip <drive> [options]        Rip audio tracks (see --format)\n"
        "  cdreader offset <drive> [options]     Detect the drive read offset (AccurateRip)\n"
        "  cdreader offsets                      List the read offsets saved per drive\n"
        "\n"
        "Rip options:\n"
        "  -o, --output <dir>    Output directory (default: \"Artist - Album\" from CDDB,\n"
        "                        otherwise cd_<CDDB id>)\n"
        "                        A CUE sheet (<album>.cue) is written next to the audio\n"
        "  -f, --format <name>   Output format: %s (default: wav)\n"
        "                        oggflac = FLAC in an Ogg container (.oga, lossless)\n"
        "                        alac = Apple Lossless in an MP4 container (.m4a)\n"
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
        "      --offset auto     Use the offset saved for this drive; if there is none,\n"
        "                        detect it first with AccurateRip (like 'offset --save',\n"
        "                        the disc must be in the database) and save it\n"
        "      --verify          Read everything twice and compare (slower)\n"
        "      --no-c2           Do not use C2 error pointers. By default drives that\n"
        "                        report them (MODE SENSE) read with C2 bits, and\n"
        "                        sectors with C2 errors are re-read (up to --retries\n"
        "                        times); unresolved ones are logged as suspicious\n"
        "      --cache <mode>    Defeat the drive's read cache before re-reads (verify\n"
        "                        second read, retries, C2 re-reads) so that they read\n"
        "                        the disc again: auto (default: test the drive once,\n"
        "                        a few seconds), fua (READ(12) with FUA, fast, only\n"
        "                        some drives), flush (read more than the cache size\n"
        "                        elsewhere first, any drive, slower), none\n"
        "      --no-accuraterip  Do not look up the AccurateRip database after ripping\n"
        "      --single-file     Rip the tracks into one file (an image of the disc);\n"
        "                        the CUE sheet then marks the track positions. FLAC\n"
        "                        and Ogg FLAC images also carry the CUE sheet inside\n"
        "                        the file, Matroska images a chapter per track\n"
        "      --no-cue-file     Do not write the external .cue file\n"
        "      --no-isrc         Do not read the MCN (catalog number) and the ISRCs\n"
        "                        from the Q sub-channel (some drives are slow at it)\n"
        "      --no-gaps         Do not detect pregaps (INDEX 00) and index points from\n"
        "                        the Q sub-channel (takes a few seconds per track); the\n"
        "                        CUE sheet then has INDEX 01 only\n"
        "      --htoa            Also rip the hidden track before track 1 (HTOA) as\n"
        "                        track 00 (single-file images always include it)\n"
        "\n"
        "Offset options (reads up to 3 tracks; the offset is confirmed when 2 tracks\n"
        "match the AccurateRip database at the same offset):\n"
        "  -t, --track <list>    Tracks to compare, e.g. 3 or 2,5 (default: chosen by\n"
        "                        confidence, avoiding the first and last track)\n"
        "      --range <n>       Offsets to try, -n..+n samples (default: 3000)\n"
        "  -r, --retries <n>     Retries per failing read (default: 5)\n"
        "      --save            Save a confirmed offset for this drive (used by\n"
        "                        rip --offset auto). Saved offsets: %s\n"
        "\n"
        "CDDB options (rip and toc):\n"
        "      --no-cddb         Do not look up the disc online\n"
        "      --cddb-server <url>  CDDB HTTP server (default: %s)\n"
        "      --cddb-match <n>  Use the n-th match when there are several (default: 1)\n"
        "      --cddb-hello <user@host>  User and host sent in the CDDB greeting\n"
        "                        (default: cdreader@localhost)\n"
        "\n"
        "<drive> is a drive letter such as D or D:\n",
        kVersion, formatList().c_str(), offsetStorePath().u8string().c_str(), cdr::kDefaultCddbServer);
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

// Reads the MCN and the ISRCs of `tracks` (all optional). Never throws.
cdr::DiscCodes readDiscCodes(cdr::CdDrive& drive, const std::vector<cdr::Track>& tracks, bool enabled) {
    if (!enabled) return {};
    std::printf("Reading MCN / ISRC ...\r");
    std::fflush(stdout);
    cdr::DiscCodes codes = cdr::readDiscCodes(drive, tracks);
    std::printf("                      \r");
    return codes;
}

// Detects pregaps / index points / the HTOA, or takes the HTOA from the TOC
// alone when disabled. Never throws.
cdr::DiscGaps detectGaps(cdr::CdDrive& drive, const cdr::Toc& toc, bool enabled) {
    if (!enabled) return cdr::gapsFromToc(toc);
    cdr::GapDetectionOptions options;
    options.progress = [](int track, int last) {
        std::printf("Detecting gaps: track %d of %d   \r", track, last);
        std::fflush(stdout);
    };
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc, options);
    std::printf("                                   \r");
    return gaps;
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

void printToc(const cdr::Toc& toc, const cdr::AlbumMetadata& album, const cdr::DiscCodes& codes,
              const cdr::DiscGaps& gaps, FILE* out) {
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
    std::fprintf(out, "\n");
    for (const std::string& line : codes.logLines()) std::fprintf(out, "%s\n", line.c_str());
    std::fprintf(out, "\n");
    for (const std::string& line : gaps.logLines()) std::fprintf(out, "%s\n", line.c_str());
}

int cmdDrives() {
    const std::vector<char> letters = cdr::win::listOpticalDrives();
    const cdr::DriveOffsetStore offsets = loadOffsetStore();
    if (letters.empty()) {
        std::printf("No optical drives found.\n");
        return 1;
    }
    for (char letter : letters) {
        try {
            OpenedDrive d = openDrive(letter);
            // The cache test needs a disc and takes seconds: rip --cache auto runs it.
            const cdr::DriveCapabilities caps = d.drive->readCapabilities();
            const std::string cache = !caps.valid      ? "cache size unknown"
                                      : caps.bufferKB ? "cache " + std::to_string(caps.bufferKB) + " KB"
                                                      : "cache size not reported";
            const cdr::SavedDriveOffset* saved = offsets.find(cdr::driveOffsetKey(d.info));
            const std::string offset =
                saved ? ", saved read offset " + std::string(saved->offset > 0 ? "+" : "") +
                            std::to_string(saved->offset)
                      : "";
            std::printf("%c:  %s  [%s, %s%s]\n", letter, d.info.displayName().c_str(),
                        cdr::checkC2(caps, true).logLine().c_str(), cache.c_str(), offset.c_str());
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
    bool discCodes = true;
    bool gapDetection = true;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--no-isrc") discCodes = false;
        else if (args[i] == "--no-gaps") gapDetection = false;
        else if (!parseCddbOption(args, i, cddb)) throw UsageError("unknown option '" + args[i] + "'");
    }

    OpenedDrive d = openDrive(letter);
    std::printf("Drive: %s\n", d.info.displayName().c_str());
    std::printf("%s\n", cdr::checkC2(*d.drive, true).logLine().c_str());
    const cdr::Toc toc = readTocOrExplain(*d.drive);
    const cdr::DiscCodes codes = readDiscCodes(*d.drive, toc.tracks, discCodes);
    const cdr::DiscGaps gaps = detectGaps(*d.drive, toc, gapDetection);
    const cdr::CddbLookupResult found = lookupDisc(toc, cddb);
    if (found.found) {
        printAlbum(found.album, stdout);
        std::printf("\n");
    }
    printToc(toc, found.album, codes, gaps, stdout);
    return 0;
}

int cmdOffsets() {
    const cdr::DriveOffsetStore store = loadOffsetStore();
    std::printf("Saved read offsets (%s):\n", offsetStorePath().u8string().c_str());
    if (store.entries().empty()) std::printf("  none (use 'cdreader offset <drive> --save' or 'rip --offset auto')\n");
    for (const auto& [key, value] : store.entries())
        std::printf("  %+5d  %s%s%s\n", value.offset, key.c_str(), value.note.empty() ? "" : "  ", value.note.c_str());
    return 0;
}

// Runs the offset detection with a progress line on the console. Never
// throws for network or read problems (see the status).
cdr::OffsetDetection detectOffset(cdr::CdDrive& drive, const cdr::Toc& toc, cdr::OffsetDetectOptions options) {
    std::printf("Detecting the read offset with AccurateRip (offsets -%u..+%u)...\n", options.maxOffset,
                options.maxOffset);
    std::fflush(stdout);
    int lastPercent = -1;
    int lastStep = 0;
    options.progress = [&](const cdr::OffsetDetectProgress& p) {
        const int percent = p.totalSectors ? int(uint64_t(p.doneSectors) * 100 / p.totalSectors) : 100;
        if (percent == lastPercent && p.step == lastStep) return;
        std::printf("\rReading track %02d (%d of at most %d)  %3d%%", p.track, p.step, p.steps, percent);
        std::fflush(stdout);
        lastPercent = percent;
        lastStep = p.step;
    };
    cdr::win::WinHttpClient http(std::string("cdreader/") + kVersion);
    const cdr::OffsetDetection d = cdr::detectReadOffset(drive, toc, http, options);
    if (lastStep) std::printf("\n");
    return d;
}

std::string driveNote(const cdr::OffsetDetection& d) { return "auto-detected: " + d.agreement(); }

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
                 letter + ": --save' or 'rip --offset auto'.");
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
    bool discCodes = true;
    bool gapDetection = true;
    bool ripHtoa = false;
    bool useC2 = true;
    cdr::CacheSetting cacheSetting = cdr::CacheSetting::Auto;
    cdr::EncoderSettings encoder;
    bool offsetAuto = false;

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
        else if (a == "--offset") {
            const std::string& v = value();
            offsetAuto = v == "auto";
            if (!offsetAuto) options.readOffsetSamples = parseSignedInt(v, "read offset");
        }
        else if (a == "--no-cue-file") cueFile = false;
        else if (a == "--no-isrc") discCodes = false;
        else if (a == "--no-gaps") gapDetection = false;
        else if (a == "--htoa") ripHtoa = true;
        else if (a == "--verify") options.verify = true;
        else if (a == "--no-c2") useC2 = false;
        else if (a == "--cache") {
            if (!cdr::parseCacheSetting(value(), cacheSetting))
                throw UsageError("invalid --cache mode '" + args[i] + "' (auto, fua, flush or none)");
        }
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

    // Read offset (#37): manual, saved for this drive, or detected now.
    cdr::ReadOffsetSource offsetSource;
    std::vector<std::string> detectionLog;
    if (offsetAuto) {
        const cdr::DriveOffsetStore store = loadOffsetStore();
        if (const cdr::SavedDriveOffset* saved = store.find(cdr::driveOffsetKey(d.info))) {
            options.readOffsetSamples = saved->offset;
            offsetSource.kind = cdr::ReadOffsetSource::Kind::Saved;
            offsetSource.detail = d.info.displayName() + (saved->note.empty() ? "" : "; " + saved->note);
        } else {
            cdr::OffsetDetectOptions detect;
            detect.rip.maxRetries = options.maxRetries;
            const cdr::OffsetDetection found = detectOffset(*d.drive, toc, detect);
            for (const std::string& l : found.logLines()) std::printf("%s\n", l.c_str());
            if (!found.detected())
                throw std::runtime_error("--offset auto: the read offset of this drive could not be detected with "
                                         "this disc. Rip with --offset <n>, or try 'cdreader offset' with another "
                                         "(popular) disc.");
            options.readOffsetSamples = found.offset;
            offsetSource.kind = cdr::ReadOffsetSource::Kind::Detected;
            offsetSource.detail = found.agreement();
            detectionLog = found.logLines();
            saveDriveOffset(d.info, {found.offset, driveNote(found)});
        }
    }

    // C2 error pointers (#33): only for drives that report them.
    const cdr::DriveCapabilities caps = d.drive->readCapabilities();
    const cdr::C2Availability c2 = cdr::checkC2(caps, useC2);
    options.useC2 = c2.usable();
    const std::string c2Line = c2.logLine() + (useC2 ? "" : " (--no-c2)");
    std::printf("%s\n", c2Line.c_str());

    // Drive cache defeat for re-reads (#34).
    if (cacheSetting == cdr::CacheSetting::Auto) {
        std::printf("Testing the drive cache...");
        std::fflush(stdout);
    }
    const cdr::DriveCacheCheck cache = cdr::checkDriveCache(*d.drive, toc, cacheSetting, cdr::steadyClock(), &caps);
    if (cacheSetting == cdr::CacheSetting::Auto) std::printf("\r");
    std::printf("%s\n", cache.logLine().c_str());
    options.cacheDefeat = cache.method;
    options.flushSectors = cache.flushSectors;

    // MCN / ISRC are optional too: a drive that cannot read them only means
    // no CATALOG / ISRC lines and tags.
    const cdr::DiscCodes codes = readDiscCodes(*d.drive, selected, discCodes);
    if (codes.mcn.found()) std::printf("MCN: %s\n", codes.mcn.value.c_str());

    // Pregaps / index points for the CUE sheet. The HTOA (audio before track
    // 1) is part of a single-file image; per-track rips save it as track 00
    // only with --htoa.
    const cdr::DiscGaps gaps = detectGaps(*d.drive, toc, gapDetection);
    const bool htoaSelected = gaps.hasHtoa() && selected.front().number == 1;
    const bool withHtoa = htoaSelected && (singleFile || ripHtoa);
    if (gapDetection) {
        int pregaps = 0;
        for (const cdr::TrackIndexes& t : gaps.tracks) pregaps += t.track > 1 && t.pregapSectors ? 1 : 0;
        if (gaps.status == cdr::DiscGaps::Status::Unsupported)
            std::printf("Gap detection: %s\n", gaps.detail.c_str());
        else
            std::printf("Gap detection: %d pregap(s)%s (%.1f s)\n", pregaps,
                        gaps.status == cdr::DiscGaps::Status::Partial ? ", some tracks unknown (see rip.log)" : "",
                        gaps.seconds);
    }
    if (gaps.hasHtoa())
        std::printf("Hidden track before track 1 (HTOA): %s%s\n", cdr::formatMsf(gaps.htoaSectors).c_str(),
                    withHtoa                 ? (singleFile ? " (included in the image)" : " (ripped as track 00)")
                    : htoaSelected && !singleFile ? " (not ripped; use --htoa)"
                                                 : "");
    else if (ripHtoa)
        std::printf("--htoa: this disc has no hidden track before track 1\n");

    // Metadata is optional: a failed lookup only means generic names.
    const cdr::CddbLookupResult found = lookupDisc(toc, cddb);
    cdr::AlbumMetadata album;
    if (found.found) {
        album = found.album;
        printAlbum(album, stdout);
    }
    album.discId = hex32(toc.cddbId());
    codes.applyTo(album);

    // In single-file mode the tracks are written back to back, which is only
    // an image of the disc if they are adjacent on it.
    const std::string extension = probe->extension();
    const std::string encoderText = probe->encoderDescription();
    const std::string albumBase = cdr::albumFileBase(album, "CDImage");
    const std::string imageName = albumBase + "." + extension;
    std::vector<cdr::CueTrack> cueTracks;
    if (singleFile) {
        try {
            cueTracks = cdr::singleFileCueTracks(selected, imageName, album, gaps, withHtoa);
        } catch (const std::invalid_argument& e) {
            throw UsageError(std::string("--single-file needs consecutive audio tracks (") + e.what() + ")");
        }
    }

    const fs::path dir = fs::u8path(outputDir.empty() ? cdr::albumDirectoryName(album) : outputDir);
    fs::create_directories(dir);

    std::printf("Read offset correction: %+d samples (%s)\n", options.readOffsetSamples,
                offsetSource.describe().c_str());
    std::printf("Format: %s\n", format.c_str());
    if (!encoderText.empty()) std::printf("Encoder: %s\n", encoderText.c_str());
    std::printf("Output: %s\n\n", dir.u8string().c_str());

    std::ofstream log(dir / "rip.log");
    log << "cdreader " << kVersion << " rip log\n"
        << "Drive: " << d.info.displayName() << "\n"
        << c2Line << "\n";
    for (const std::string& l : cache.logLines()) log << l << "\n";
    log << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n"
        << cdr::readOffsetLogLine(options.readOffsetSamples, offsetSource) << "\n";
    for (const std::string& l : detectionLog) log << l << "\n";
    log << "Format: " << format << "\n";
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
        for (const std::string& l : codes.logLines()) log << l << "\n";
        log << "\n";
        for (const std::string& l : gaps.logLines()) log << l << "\n";
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
            embedded.mcn = album.mcn;
            for (const cdr::Track& t : selected) embedded.totalSectors += t.lengthSectors;
            if (withHtoa) embedded.totalSectors += gaps.htoaSectors;
            embedded.text = cdr::formatCueSheet(album, cueTracks);
            writer->setEmbeddedCueSheet(embedded);
        }
        writer->open(dir / fs::u8path(imageName), album.forTrack(0, toc.lastTrack));
        log << "Single file: " << imageName << "\n";
        if (writer->canEmbedCueSheet()) {
            // FLAC: CUESHEET block + tag; Matroska: chapters (#23).
            const char* what = extension != "mka" ? "CUESHEET block and tag"
                               : withHtoa         ? "Matroska chapters (one per track, plus the HTOA)"
                                                  : "Matroska chapters (one per track)";
            std::printf("Embedded CUE sheet: %s in %s\n\n", what, imageName.c_str());
            log << "Embedded CUE sheet: " << what << "\n";
        }
    }
    // The HTOA comes first (track 00): in the image before track 1, or in its own file.
    std::vector<cdr::Track> parts = selected;
    if (withHtoa) parts.insert(parts.begin(), cdr::htoaTrack(toc));
    std::string htoaFile;
    for (const cdr::Track& t : parts) {
        const bool htoa = t.number == 0;
        std::string name;
        if (singleFile) {
            char base[32];
            std::snprintf(base, sizeof base, htoa ? "Track %02d (HTOA)" : "Track %02d", t.number);
            name = base;
        } else {
            writer = cdr::createAudioWriter(format, encoder);
            cdr::TrackMetadata metadata = album.forTrack(t.number, toc.lastTrack);
            if (htoa && !album.title.empty()) metadata.title = "Hidden Track";  // "00 - Hidden Track"; else Track00
            name = cdr::trackFileBaseName(metadata) + "." + extension;
            writer->open(dir / fs::u8path(name), metadata);
            if (htoa) htoaFile = name;
            else trackFiles.push_back(name);
        }
        int lastPercent = -1;
        // No AccurateRip checksums for the HTOA: the database has none.
        cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, htoa ? selected.front() : t);
        cdr::TrackRipResult r = ripper.ripTrack(
            t,
            [&](const uint8_t* pcm, size_t bytes) {
                writer->write(pcm, bytes);
                if (!htoa) ar.update(pcm, bytes);
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

        const std::string status = r.status();
        std::string notes;
        if (r.retries) notes += "  (" + std::to_string(r.retries) + " retries)";
        if (r.c2ErrorSectors)
            notes += "  (C2 errors in " + std::to_string(r.c2ErrorSectors) + " sector(s), " +
                     std::to_string(r.c2Rereads) + " re-reads)";
        if (r.cacheDefeats)
            notes += "  (" + std::to_string(r.cacheDefeats) +
                     (r.flushSectorsRead ? " cache flushes)" : " FUA commands)");
        std::printf("  CRC32 %s  %s%s\n", hex32(r.crc32).c_str(), status.c_str(), notes.c_str());
        if (!r.c2Fallback.empty()) std::printf("  C2 reads given up: %s\n", r.c2Fallback.c_str());
        if (!r.cacheFallback.empty()) std::printf("  FUA given up: %s\n", r.cacheFallback.c_str());
        log << name << "  CRC32 " << hex32(r.crc32) << "  retries " << r.retries << "  " << status;
        if (r.paddedSamples) log << "  (" << r.paddedSamples << " samples outside the disc padded with silence)";
        log << "\n";
        for (const std::string& l : cdr::c2LogLines(r)) log << l << "\n";
        for (const std::string& l : cdr::cacheLogLines(r)) log << l << "\n";
        if (!r.clean()) ++problems;
        if (!htoa) arTracks.push_back({t, ar.v1(), ar.v2()});
    }
    if (singleFile) {
        writer->close();
        log << imageName << "  CRC32 " << hex32(imageCrc.value()) << "\n";
    }

    if (cueFile) {
        const std::string cueName = albumBase + ".cue";
        const std::string cue =
            cdr::formatCueSheet(album, singleFile ? cueTracks
                                                  : cdr::perTrackCueTracks(selected, trackFiles, album, gaps, htoaFile));
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

// Detects the read offset (#37): reads a few tracks and compares their
// checksums at every candidate offset with the AccurateRip database; the
// offset is confirmed when two tracks agree. --save stores it for the drive.
int cmdOffset(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("offset needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    int range = 3000;
    bool save = false;
    cdr::OffsetDetectOptions options;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
            return args[++i];
        };
        if (a == "-t" || a == "--track" || a == "--tracks") {
            const std::string& spec = value();
            options.tracks.clear();
            // Keep the order given ("5,2"), ranges ascending.
            size_t pos = 0;
            while (pos <= spec.size()) {
                const size_t comma = spec.find(',', pos);
                for (int t : parseTrackList(spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)))
                    options.tracks.push_back(t);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (a == "--range") range = parseInt(value(), "offset range");
        else if (a == "-r" || a == "--retries") options.rip.maxRetries = parseInt(value(), "retry count");
        else if (a == "--save") save = true;
        else throw UsageError("unknown option '" + a + "'");
    }
    if (range < 1 || range > int(100 * cdr::kSamplesPerSector)) throw UsageError("offset range out of range");
    options.maxOffset = uint32_t(range);
    if (!options.tracks.empty()) {
        options.maxTracks = int(options.tracks.size());
        // Tracks picked by hand: one track is enough if it is the only one.
        if (options.tracks.size() == 1) options.singleTrackMinConfidence = 1;
    }

    OpenedDrive d = openDrive(letter);
    const cdr::Toc toc = readTocOrExplain(*d.drive);
    for (int n : options.tracks) {
        const cdr::Track* t = toc.findTrack(n);
        if (!t || !t->isAudio) throw UsageError("track " + std::to_string(n) + " is not an audio track");
    }
    std::printf("Drive: %s\n", d.info.displayName().c_str());
    std::printf("AccurateRip disc id: %s\n", cdr::AccurateRipDiscId::fromToc(toc).toString().c_str());
    if (const cdr::SavedDriveOffset* saved = loadOffsetStore().find(cdr::driveOffsetKey(d.info)))
        std::printf("Saved read offset for this drive: %+d%s%s\n", saved->offset, saved->note.empty() ? "" : "  ",
                    saved->note.c_str());

    const cdr::OffsetDetection found = detectOffset(*d.drive, toc, options);
    std::printf("\n");
    for (const std::string& l : found.logLines()) std::printf("%s\n", l.c_str());
    if (!found.detected()) {
        if (save) std::printf("Nothing saved.\n");
        return 1;
    }
    std::printf("\nRead offset: %+d  (use: cdreader rip %c: --offset %d%s)\n", found.offset, letter, found.offset,
                save ? ", or --offset auto" : "; --save stores it for --offset auto");
    if (save) saveDriveOffset(d.info, {found.offset, driveNote(found)});
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
        if (args[0] == "offsets") return cmdOffsets();
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
