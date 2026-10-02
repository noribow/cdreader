// cdreader - command line CD audio ripper for Windows.

#include <windows.h>
#include <shellapi.h>

#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"
#include "cdreader/wav_writer.h"
#include "spti_transport.h"

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
        "  cdreader toc <drive>                  Show the table of contents\n"
        "  cdreader rip <drive> [options]        Rip audio tracks to WAV\n"
        "\n"
        "Rip options:\n"
        "  -o, --output <dir>    Output directory (default: cd_<CDDB id>)\n"
        "  -t, --tracks <list>   Tracks to rip, e.g. 1,3-5 (default: all audio tracks)\n"
        "  -r, --retries <n>     Retries per failing read (default: 5)\n"
        "      --verify          Read everything twice and compare (slower)\n"
        "\n"
        "<drive> is a drive letter such as D or D:\n",
        kVersion);
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

int parseInt(const std::string& s, const char* what) {
    try {
        size_t used = 0;
        int v = std::stoi(s, &used);
        if (used == s.size() && v >= 0) return v;
    } catch (const std::exception&) {
    }
    throw UsageError(std::string("invalid ") + what + " '" + s + "'");
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

void printToc(const cdr::Toc& toc, FILE* out) {
    std::fprintf(out, "CDDB disc id: %s\n", hex32(toc.cddbId()).c_str());
    std::fprintf(out, "Track  Start LBA   Length     Type\n");
    for (const cdr::Track& t : toc.tracks) {
        std::fprintf(out, "  %2d   %9u   %s   %s%s\n", t.number, t.startLba, cdr::formatMsf(t.lengthSectors).c_str(),
                     t.isAudio ? "audio" : "data", t.preEmphasis ? " (pre-emphasis)" : "");
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
    if (args.size() != 2) throw UsageError("toc takes exactly one drive argument");
    OpenedDrive d = openDrive(parseDriveLetter(args[1]));
    std::printf("Drive: %s\n", d.info.displayName().c_str());
    printToc(readTocOrExplain(*d.drive), stdout);
    return 0;
}

int cmdRip(const std::vector<std::string>& args) {
    if (args.size() < 2) throw UsageError("rip needs a drive argument");
    const char letter = parseDriveLetter(args[1]);
    std::string outputDir;
    std::set<int> wanted;
    cdr::RipOptions options;

    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) throw UsageError(a + " needs a value");
            return args[++i];
        };
        if (a == "-o" || a == "--output") outputDir = value();
        else if (a == "-t" || a == "--tracks") wanted = parseTrackList(value());
        else if (a == "-r" || a == "--retries") options.maxRetries = parseInt(value(), "retry count");
        else if (a == "--verify") options.verify = true;
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

    const fs::path dir = fs::u8path(outputDir.empty() ? "cd_" + hex32(toc.cddbId()) : outputDir);
    fs::create_directories(dir);

    std::printf("Drive: %s\n", d.info.displayName().c_str());
    std::printf("Output: %s\n\n", dir.u8string().c_str());

    std::ofstream log(dir / "rip.log");
    log << "cdreader " << kVersion << " rip log\n"
        << "Drive: " << d.info.displayName() << "\n"
        << "Mode: " << (options.verify ? "verify (double read)" : "burst") << ", retries " << options.maxRetries
        << "\n\n";
    {
        char line[256];
        log << "CDDB disc id: " << hex32(toc.cddbId()) << "\n";
        for (const cdr::Track& t : toc.tracks) {
            std::snprintf(line, sizeof line, "Track %2d  LBA %7u  %s  %s\n", t.number, t.startLba,
                          cdr::formatMsf(t.lengthSectors).c_str(), t.isAudio ? "audio" : "data");
            log << line;
        }
        log << "\n";
    }

    cdr::Ripper ripper(*d.drive, options);
    int problems = 0;
    for (const cdr::Track& t : selected) {
        char name[32];
        std::snprintf(name, sizeof name, "Track%02d.wav", t.number);
        const fs::path file = dir / name;

        cdr::WavWriter wav;
        wav.open(file);
        int lastPercent = -1;
        cdr::TrackRipResult r = ripper.ripTrack(
            t, [&](const uint8_t* pcm, size_t bytes) { wav.write(pcm, bytes); },
            [&](uint32_t done, uint32_t total) {
                int percent = total ? int(uint64_t(done) * 100 / total) : 100;
                if (percent != lastPercent) {
                    std::printf("\rTrack %02d [%s]  %3d%%", t.number, cdr::formatMsf(t.lengthSectors).c_str(), percent);
                    std::fflush(stdout);
                    lastPercent = percent;
                }
            });
        wav.close();

        const std::string status = r.clean() ? "OK" : std::to_string(r.unreadableSectors) + " unreadable sector(s)";
        std::printf("  CRC32 %s  %s%s\n", hex32(r.crc32).c_str(), status.c_str(),
                    r.retries ? ("  (" + std::to_string(r.retries) + " retries)").c_str() : "");
        log << name << "  CRC32 " << hex32(r.crc32) << "  retries " << r.retries << "  " << status << "\n";
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
