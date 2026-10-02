// Decodes AlacWriter output (.m4a) with FFmpeg, an ALAC decoder independent
// of ours:
//   ffmpeg -i x.m4a -f s16le  must give the input PCM bit for bit, for every
//                             synthetic signal (silence, full scale, noise,
//                             short and odd lengths, music-like, ...),
//   ffprobe                   must report codec alac, 44.1 kHz stereo, the
//                             exact duration and the tags.
// Also prints the size of the ALAC frames next to our FLAC frames and
// FFmpeg's own ALAC encoder on the same signals.
//
// Usage: cdreader_alac_ffmpeg_check [--ffmpeg P] [--ffprobe P]
// Exits with 77 (reported as "skipped" by ctest) when ffmpeg is not given.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "alac_decoder.h"
#include "cdreader/alac_writer.h"
#include "cdreader/flac_writer.h"
#include "test_signals.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string readText(const fs::path& path) {
    const std::vector<uint8_t> v = readFile(path);
    return std::string(v.begin(), v.end());
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

int run(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());  // cmd.exe strips the outer quotes
#else
    return std::system(command.c_str());
#endif
}

// Bytes of the FLAC frames, i.e. the file without its metadata blocks.
uint64_t flacFrameBytes(const std::vector<uint8_t>& f) {
    size_t pos = 4;
    for (bool last = false; !last && pos + 4 <= f.size();) {
        last = (f[pos] & 0x80) != 0;
        pos += 4 + (size_t(f[pos + 1]) << 16 | size_t(f[pos + 2]) << 8 | f[pos + 3]);
    }
    return pos < f.size() ? f.size() - pos : 0;
}

// Bytes of the media data (mdat payload) of an MP4 file.
uint64_t mdatBytes(const std::vector<uint8_t>& f) {
    uint64_t pos = 0;
    while (pos + 8 <= f.size()) {
        uint64_t size = 0;
        for (int i = 0; i < 4; ++i) size = size << 8 | f[pos + i];
        unsigned header = 8;
        if (size == 1 && pos + 16 <= f.size()) {
            size = 0;
            for (int i = 8; i < 16; ++i) size = size << 8 | f[pos + i];
            header = 16;
        }
        if (size < header) break;
        if (std::string(reinterpret_cast<const char*>(&f[pos + 4]), 4) == "mdat") return size - header;
        pos += size;
    }
    return 0;
}

cdr::TrackMetadata metadata(const std::string& title) {
    cdr::TrackMetadata m;
    m.trackNumber = 3;
    m.trackTotal = 12;
    m.title = title;
    m.artist = "\xE3\x82\xA2\xE3\x83\xBC\xE3\x83\x86\xE3\x82\xA3\xE3\x82\xB9\xE3\x83\x88";  // アーティスト
    m.album = "Album";
    m.albumArtist = "Album Artist";
    m.year = "1999";
    m.genre = "Rock";
    m.discId = "0A0B0C03";
    return m;
}

void encode(cdr::AudioWriter& writer, const fs::path& path, const cdr::TrackMetadata& meta,
            const std::vector<uint8_t>& pcm) {
    writer.open(path, meta);
    size_t pos = std::min<size_t>(pcm.size(), 7);  // odd split first: partial samples are buffered
    writer.write(pcm.data(), pos);
    for (; pos < pcm.size(); pos += 2352) writer.write(pcm.data() + pos, std::min<size_t>(2352, pcm.size() - pos));
    writer.close();
}

// "key=value" lines of ffprobe's default output, keys lower-cased.
std::map<std::string, std::string> probe(const std::string& ffprobe, const fs::path& file, const fs::path& out) {
    std::map<std::string, std::string> values;
    if (run(quote(fs::u8path(ffprobe)) + " -v error -show_entries "
                                         "stream=codec_name,sample_rate,channels,duration_ts,bits_per_raw_sample:"
                                         "stream_tags:format_tags -of default=noprint_wrappers=1 " +
            quote(file) + " > " + quote(out)) != 0)
        return values;
    const std::string text = readText(out);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            if (key.rfind("TAG:", 0) == 0) key = key.substr(4);
            for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
            values.emplace(key, line.substr(eq + 1));
        }
        pos = end + 1;
    }
    return values;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ffmpeg, ffprobe;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--ffmpeg") ffmpeg = argv[i + 1];
        else if (option == "--ffprobe") ffprobe = argv[i + 1];
    }
    if (ffmpeg.empty()) {
        std::printf("ffmpeg not available - skipping\n");
        return 77;
    }
    const std::string ffmpegCmd = quote(fs::u8path(ffmpeg)) + " -v error -nostdin -y";
    const fs::path dir = fs::temp_directory_path() / "cdreader_alac_ffmpeg_check";
    fs::create_directories(dir);

    std::vector<testsig::Signal> signals = testsig::all();
    signals.push_back(testsig::tonesPcm(44100 * 5));
    {
        testsig::Signal longer = testsig::music(44100 * 20 + 333);
        longer.name = "music_20s";
        signals.push_back(longer);
    }

    int failures = 0;
    uint64_t totalPcm = 0, totalAlac = 0, totalFlac = 0, totalFfmpegAlac = 0;
    std::printf("%-6s %-24s %9s %10s %10s %10s %11s\n", "", "signal", "samples", "ALAC", "FLAC", "ffmpeg", "ALAC/FLAC");
    for (const testsig::Signal& s : signals) {
        const fs::path m4a = dir / (s.name + ".m4a");
        const fs::path raw = dir / (s.name + ".raw");
        const cdr::TrackMetadata meta = metadata(s.name + " \xE6\x9B\xB2");  // + 曲
        unsigned escaped = 0;
        {
            cdr::AlacWriter writer;
            encode(writer, m4a, meta, s.pcm);
            escaped = writer.escapedFrames();
        }
        const std::vector<uint8_t> file = readFile(m4a);

        // Our own decoder first (structure + PCM), then FFmpeg.
        bool ok = true;
        std::string why;
        try {
            if (decodeM4a(file).pcm != s.pcm) {
                ok = false;
                why = "test decoder: PCM differs";
            }
        } catch (const std::exception& e) {
            ok = false;
            why = std::string("test decoder: ") + e.what();
        }
        fs::remove(raw);
        if (run(ffmpegCmd + " -i " + quote(m4a) + " -f s16le -acodec pcm_s16le " + quote(raw)) != 0) {
            ok = false;
            why += " ffmpeg failed";
        } else if (readFile(raw) != s.pcm) {
            ok = false;
            why += " ffmpeg PCM differs (" + std::to_string(readFile(raw).size()) + " vs " +
                   std::to_string(s.pcm.size()) + " bytes)";
        }

        if (!ffprobe.empty() && !s.pcm.empty()) {
            const auto v = probe(ffprobe, m4a, dir / "probe.txt");
            auto expect = [&](const std::string& key, const std::string& value) {
                const auto it = v.find(key);
                if (it == v.end() || it->second != value) {
                    ok = false;
                    why += " ffprobe " + key + "=" + (it == v.end() ? "(missing)" : it->second);
                }
            };
            expect("codec_name", "alac");
            expect("sample_rate", "44100");
            expect("channels", "2");
            expect("bits_per_raw_sample", "16");
            expect("duration_ts", std::to_string(s.pcm.size() / 4));
            expect("title", meta.title);
            expect("artist", meta.artist);
            expect("album", "Album");
            expect("album_artist", "Album Artist");
            expect("track", "3/12");
            expect("date", "1999");
            expect("genre", "Rock");
            expect("cddb", "0A0B0C03");
        }

        // Sizes: our FLAC frames, FFmpeg's ALAC encoder (default settings).
        uint64_t flac = 0, ffmpegAlac = 0;
        {
            const fs::path flacPath = dir / (s.name + ".flac");
            cdr::FlacWriter writer;
            encode(writer, flacPath, {}, s.pcm);
            flac = flacFrameBytes(readFile(flacPath));
            fs::remove(flacPath);
        }
        if (!s.pcm.empty()) {
            const fs::path input = dir / (s.name + ".in.raw");
            const fs::path reference = dir / (s.name + ".ffmpeg.m4a");
            writeFile(input, s.pcm);
            if (run(ffmpegCmd + " -f s16le -ar 44100 -ac 2 -i " + quote(input) + " -c:a alac " + quote(reference)) == 0)
                ffmpegAlac = mdatBytes(readFile(reference));
            fs::remove(input);
            fs::remove(reference);
        }
        const uint64_t alac = mdatBytes(file);
        totalPcm += s.pcm.size();
        totalAlac += alac;
        totalFlac += flac;
        totalFfmpegAlac += ffmpegAlac;
        std::printf("%s %-24s %9zu %10ju %10ju %10ju %10.1f%%%s\n", ok ? "[ OK ]" : "[FAIL]", s.name.c_str(),
                    s.pcm.size() / 4, uintmax_t(alac), uintmax_t(flac), uintmax_t(ffmpegAlac),
                    flac ? 100.0 * double(alac) / double(flac) : 0.0,
                    escaped ? ("  (" + std::to_string(escaped) + " uncompressed frames)").c_str() : "");
        if (!ok) {
            std::printf("       %s\n", why.c_str());
            ++failures;
        }
        fs::remove(m4a);
        fs::remove(raw);
    }
    fs::remove(dir / "probe.txt");
    fs::remove(dir);
    std::printf("\nTotal: PCM %ju bytes, ALAC %ju (%.1f%%), FLAC %ju (%.1f%%), ffmpeg ALAC %ju (%.1f%%)\n",
                uintmax_t(totalPcm), uintmax_t(totalAlac), 100.0 * double(totalAlac) / double(totalPcm),
                uintmax_t(totalFlac), 100.0 * double(totalFlac) / double(totalPcm), uintmax_t(totalFfmpegAlac),
                100.0 * double(totalFfmpegAlac) / double(totalPcm));
    std::printf("%s (%zu signals)\n", failures ? "FAILED" : "PASSED", signals.size());
    return failures ? 1 : 0;
}
