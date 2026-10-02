// Round trip through the reference FLAC tool: every synthetic signal is encoded
// with FlacWriter, then checked with `flac -t` (CRCs + MD5) and decoded with
// `flac -d`; the decoded PCM must be bit-identical to the input.
//
// Usage: cdreader_flac_roundtrip [path to flac executable]
// Exits with 77 (reported as "skipped" by ctest) when no flac tool is given.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cdreader/flac_writer.h"
#include "test_signals.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Size of the audio frames, i.e. the file without its metadata blocks.
uintmax_t frameBytes(const fs::path& path) {
    const std::vector<uint8_t> f = readFile(path);
    size_t pos = 4;
    for (bool last = false; !last && pos + 4 <= f.size();) {
        last = (f[pos] & 0x80) != 0;
        pos += 4 + (size_t(f[pos + 1]) << 16 | size_t(f[pos + 2]) << 8 | f[pos + 3]);
    }
    return pos < f.size() ? f.size() - pos : 0;
}

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

int run(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());  // cmd.exe strips the outer quotes
#else
    return std::system(command.c_str());
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]).empty()) {
        std::printf("flac executable not available - skipping\n");
        return 77;
    }
    const std::string flac = quote(fs::u8path(argv[1]));
    const fs::path dir = fs::temp_directory_path() / "cdreader_flac_roundtrip";
    fs::create_directories(dir);

    int failures = 0;
    const std::vector<testsig::Signal> signals = testsig::all();
    for (const testsig::Signal& s : signals) {
        const fs::path encoded = dir / (s.name + ".flac");
        const fs::path decoded = dir / (s.name + ".raw");
        cdr::TrackMetadata meta;
        meta.title = s.name;
        meta.trackNumber = 1;
        {
            cdr::FlacWriter writer;
            writer.open(encoded, meta);
            // Feed CD sector sized chunks, plus an odd split to exercise partial samples.
            size_t pos = 0;
            if (s.pcm.size() > 7) {
                writer.write(s.pcm.data(), 7);
                pos = 7;
            }
            for (; pos < s.pcm.size(); pos += 2352)
                writer.write(s.pcm.data() + pos, std::min<size_t>(2352, s.pcm.size() - pos));
            writer.close();
        }

        bool ok = run(flac + " -s -t " + quote(encoded)) == 0;
        ok = ok && run(flac + " -s -d -f --force-raw-format --endian=little --sign=signed -o " + quote(decoded) +
                       " " + quote(encoded)) == 0;
        ok = ok && readFile(decoded) == s.pcm;

        // For comparison: the reference encoder at its default level (-5), without padding.
        const fs::path reference = dir / (s.name + ".ref.flac");
        std::ofstream(decoded, std::ios::binary).write(reinterpret_cast<const char*>(s.pcm.data()),
                                                       std::streamsize(s.pcm.size()));
        const bool haveReference =
            !s.pcm.empty() && run(flac + " -s -f -5 --no-padding --force-raw-format --endian=little --sign=signed"
                                         " --channels=2 --bps=16 --sample-rate=44100 -o " +
                                  quote(reference) + " " + quote(decoded)) == 0;
        std::printf("%s %-24s %8zu samples  frames %8ju bytes", ok ? "[ OK ]" : "[FAIL]", s.name.c_str(),
                    s.pcm.size() / 4, frameBytes(encoded));
        if (haveReference) std::printf("  (flac -5: %ju)", frameBytes(reference));
        std::printf("\n");
        if (!ok) ++failures;
        fs::remove(encoded);
        fs::remove(decoded);
        fs::remove(reference);
    }
    fs::remove(dir);
    std::printf("\n%s (%zu signals)\n", failures ? "FAILED" : "PASSED", signals.size());
    return failures ? 1 : 0;
}
