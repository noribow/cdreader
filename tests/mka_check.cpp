// Checks MkaWriter output (#23) with the reference tools:
//   mkvinfo (mkvtoolnix)  must parse every file without errors or warnings,
//   ffmpeg                must decode FLAC / PCM in Matroska bit-identically to
//                         the input, and Opus / Vorbis to exactly the expected
//                         length (CodecDelay, DiscardPadding) close to the input,
//   ffprobe               must read the tags and the chapters of a disc image.
//
// Usage: cdreader_mka_check [--mkvinfo P] [--ffprobe P] [--ffmpeg P]
// Exits with 77 (reported as "skipped" by ctest) when no tool is available.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/resampler.h"
#include "cdreader/toc.h"
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

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

int run(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());  // cmd.exe strips the outer quotes
#else
    return std::system(command.c_str());
#endif
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int16_t sampleAt(const std::vector<uint8_t>& pcm, size_t index) {
    return int16_t(uint16_t(pcm[2 * index] | pcm[2 * index + 1] << 8));
}

double snrDb(const std::vector<double>& reference, const std::vector<double>& actual) {
    double signal = 0, error = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        signal += reference[i] * reference[i];
        error += (actual[i] - reference[i]) * (actual[i] - reference[i]);
    }
    return error == 0 ? 999.0 : 10 * std::log10(signal / error);
}

cdr::TrackMetadata metadata() {
    cdr::TrackMetadata m;
    m.trackNumber = 3;
    m.trackTotal = 12;
    m.title = "\xE6\x9B\xB2\xE5\x90\x8D \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";  // 曲名 テスト
    m.artist = "Artist";
    m.album = "Album";
    m.albumArtist = "Album Artist";
    m.year = "1999";
    m.genre = "Rock";
    m.discId = "0A0B0C03";
    return m;
}

void encode(const std::string& format, const fs::path& path, const std::vector<uint8_t>& pcm,
            const cdr::TrackMetadata& m, const cdr::EmbeddedCueSheet* cue = nullptr) {
    std::unique_ptr<cdr::AudioWriter> writer = cdr::createAudioWriter(format);
    if (cue) writer->setEmbeddedCueSheet(*cue);
    writer->open(path, m);
    for (size_t pos = 0; pos < pcm.size(); pos += 2352 * 13)
        writer->write(pcm.data() + pos, std::min<size_t>(2352 * 13, pcm.size() - pos));
    writer->close();
}

struct Tools {
    std::string mkvinfo, ffprobe, ffmpeg;
};

struct Checker {
    fs::path dir;
    Tools tools;
    int failures = 0;

    void expect(bool ok, const std::string& what) {
        std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
        if (!ok) ++failures;
    }

    // mkvinfo must succeed without reporting an error or a warning.
    void mkvinfo(const fs::path& file, const std::string& label) {
        if (tools.mkvinfo.empty()) return;
        const fs::path out = dir / "mkvinfo.txt";
        const int status = run(quote(fs::u8path(tools.mkvinfo)) + " " + quote(file) + " > " + quote(out) + " 2>&1");
        const std::string text = lower(readText(out));
        const bool clean = text.find("warning") == std::string::npos && text.find("error") == std::string::npos &&
                           text.find("+ segment") != std::string::npos;
        expect(status == 0 && clean, "mkvinfo: " + label);
        if (status != 0 || !clean) std::printf("%s\n", readText(out).c_str());
    }

    // Decodes with ffmpeg to 16-bit PCM at the stream's own rate; empty on failure.
    std::vector<uint8_t> decode(const fs::path& file) {
        const fs::path raw = dir / "decoded.raw";
        const fs::path log = dir / "ffmpeg.txt";
        fs::remove(raw);
        const int status = run(quote(fs::u8path(tools.ffmpeg)) + " -nostdin -v error -i " + quote(file) +
                               " -f s16le -acodec pcm_s16le " + quote(raw) + " > " + quote(log) + " 2>&1");
        const std::string messages = readText(log);
        if (status != 0 || !messages.empty()) {
            std::printf("ffmpeg failed (%d): %s\n", status, messages.c_str());
            return {};
        }
        return readFile(raw);
    }

    std::string probe(const fs::path& file, const std::string& options) {
        const fs::path out = dir / "ffprobe.txt";
        const int status = run(quote(fs::u8path(tools.ffprobe)) + " -v error " + options + " " + quote(file) + " > " +
                               quote(out) + " 2>&1");
        const std::string text = readText(out);
        if (status != 0) std::printf("ffprobe failed (%d): %s\n", status, text.c_str());
        return status == 0 ? text : std::string();
    }
};

void checkTrack(Checker& c, const std::string& format, size_t samples) {
    const bool opus = format == "mka-opus", vorbis = format == "mka-vorbis";
    const testsig::Signal s = opus || vorbis ? testsig::tonesPcm(samples) : testsig::music(samples);
    const fs::path file = c.dir / "test.mka";
    encode(format, file, s.pcm, metadata());
    const std::string label = format + ", " + std::to_string(samples) + " samples";
    c.mkvinfo(file, label);

    if (!c.tools.ffmpeg.empty()) {
        const std::vector<uint8_t> pcm = c.decode(file);
        if (!opus && !vorbis) {
            c.expect(pcm == s.pcm, "ffmpeg: " + label + ": bit-identical to the input (" +
                                       std::to_string(pcm.size() / 4) + " samples)");
        } else if (opus) {
            // FFmpeg decodes Opus at 48 kHz; CodecDelay and DiscardPadding trimmed.
            const size_t expected = size_t(cdr::Resampler(44100, 48000, 2).outputLength(samples));
            c.expect(pcm.size() == expected * 4, "ffmpeg: " + label + ": " + std::to_string(pcm.size() / 4) +
                                                     " samples at 48 kHz (expected " + std::to_string(expected) + ")");
            if (samples > 44100 && pcm.size() == expected * 4) {
                std::vector<double> ref, got;
                for (size_t i = 2400; i + 2400 < expected; ++i) {
                    for (int ch = 0; ch < 2; ++ch) {
                        ref.push_back(testsig::tones(double(i) / 48000, ch));
                        got.push_back(sampleAt(pcm, 2 * i + size_t(ch)) / 32768.0);
                    }
                }
                char text[160];
                const double snr = snrDb(ref, got);
                std::snprintf(text, sizeof text, "ffmpeg: %s: SNR %.1f dB against the exact signal", label.c_str(), snr);
                c.expect(snr > 20, text);
            }
        } else {
            c.expect(pcm.size() == s.pcm.size(), "ffmpeg: " + label + ": " + std::to_string(pcm.size() / 4) +
                                                     " samples (expected " + std::to_string(samples) + ")");
            if (samples > 44100 && pcm.size() == s.pcm.size()) {
                std::vector<double> ref, got;
                for (size_t i = 0; i < samples * 2; ++i) {
                    ref.push_back(sampleAt(s.pcm, i) / 32768.0);
                    got.push_back(sampleAt(pcm, i) / 32768.0);
                }
                char text[160];
                const double snr = snrDb(ref, got);
                std::snprintf(text, sizeof text, "ffmpeg: %s: SNR %.1f dB against the input", label.c_str(), snr);
                c.expect(snr > 12, text);
            }
        }
    }

    if (!c.tools.ffprobe.empty() && samples > 44100) {
        // FFmpeg merges the tag levels (the track's TITLE / ARTIST come last)
        // and reports PART_NUMBER as "track".
        const std::string text =
            lower(c.probe(file, "-show_entries format_tags -of default=noprint_wrappers=1"));
        const cdr::TrackMetadata m = metadata();
        const std::map<std::string, std::string> want = {
            {"title", m.title},    {"artist", m.artist},          {"album", m.album},
            {"track", "3"},        {"total_parts", "12"},         {"date_released", "1999"},
            {"genre", "rock"},     {"cddb", "0a0b0c03"}};
        bool ok = !text.empty();
        for (const auto& [key, value] : want) {
            const bool found = text.find("tag:" + key + "=" + lower(value) + "\n") != std::string::npos;
            if (!found) std::printf("  missing tag %s=%s\n", key.c_str(), value.c_str());
            ok = ok && found;
        }
        c.expect(ok, "ffprobe tags: " + label);
        if (!ok) std::printf("%s\n", text.c_str());
    }
    fs::remove(file);
}

void checkImage(Checker& c, const std::string& format) {
    // Three tracks back to back, starting at sectors 0, 1501 and 2876.
    const uint32_t starts[] = {0, 1501, 2876};
    cdr::EmbeddedCueSheet cue;
    for (int i = 0; i < 3; ++i) {
        cdr::CueTrack t;
        t.number = i + 1;
        t.startSectors = starts[i];
        t.title = "Song " + std::to_string(i + 1);
        t.performer = "Performer " + std::to_string(i + 1);
        cue.tracks.push_back(t);
    }
    cue.totalSectors = 4000;
    const testsig::Signal s = testsig::music(size_t(cue.totalSectors) * cdr::kSamplesPerSector);
    cdr::TrackMetadata album = metadata();
    album.trackNumber = 0;
    album.title.clear();
    album.artist = album.albumArtist;
    const fs::path file = c.dir / "image.mka";
    encode(format, file, s.pcm, album, &cue);
    const std::string label = format + " disc image";
    c.mkvinfo(file, label);
    if (!c.tools.ffmpeg.empty() && format == "mka")
        c.expect(c.decode(file) == s.pcm, "ffmpeg: " + label + ": bit-identical to the input");

    if (!c.tools.ffprobe.empty()) {
        // One chapter per track: [CHAPTER] blocks with start / end in ns and the title tag.
        const std::string text = c.probe(file, "-show_chapters -of default=noprint_wrappers=1");
        std::istringstream lines(text);
        std::string line;
        std::vector<long long> chapterStarts, chapterEnds;
        std::vector<std::string> titles, tracks;
        bool nsTimeBase = true;
        while (std::getline(lines, line)) {
            if (line.rfind("time_base=", 0) == 0) nsTimeBase = nsTimeBase && line == "time_base=1/1000000000";
            else if (line.rfind("start=", 0) == 0) chapterStarts.push_back(std::atoll(line.c_str() + 6));
            else if (line.rfind("end=", 0) == 0) chapterEnds.push_back(std::atoll(line.c_str() + 4));
            else if (lower(line).rfind("tag:title=", 0) == 0) titles.push_back(line.substr(10));
            else if (lower(line).rfind("tag:track=", 0) == 0) tracks.push_back(line.substr(10));
        }
        bool ok = nsTimeBase && chapterStarts.size() == 3 && chapterEnds.size() == 3 && titles.size() == 3 &&
                  tracks.size() == 3;
        for (size_t i = 0; ok && i < 3; ++i) {
            const uint32_t end = i + 1 < 3 ? starts[i + 1] : cue.totalSectors;
            ok = chapterStarts[i] == std::llround(double(starts[i]) * 1e9 / 75) &&
                 chapterEnds[i] == std::llround(double(end) * 1e9 / 75) && titles[i] == "Song " + std::to_string(i + 1) &&
                 tracks[i] == std::to_string(i + 1);
        }
        c.expect(ok, "ffprobe chapters at the CUE positions: " + label);
        if (!ok) std::printf("%s\n", text.c_str());

        const std::string tags = lower(c.probe(file, "-show_entries format_tags -of default=noprint_wrappers=1"));
        const bool albumOk = tags.find("tag:title=album\n") != std::string::npos &&
                             tags.find("tag:artist=album artist\n") != std::string::npos &&
                             tags.find("tag:total_parts=12\n") != std::string::npos;
        c.expect(albumOk, "ffprobe album tags: " + label);
        if (!albumOk) std::printf("%s\n", tags.c_str());
    }
    fs::remove(file);
}

}  // namespace

int main(int argc, char** argv) {
    Checker c;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--mkvinfo") c.tools.mkvinfo = value;
        else if (option == "--ffprobe") c.tools.ffprobe = value;
        else if (option == "--ffmpeg") c.tools.ffmpeg = value;
    }
    if (c.tools.mkvinfo.empty() && c.tools.ffprobe.empty() && c.tools.ffmpeg.empty()) {
        std::printf("neither mkvinfo (mkvtoolnix) nor ffprobe / ffmpeg available - skipping\n");
        return 77;
    }
    if (c.tools.mkvinfo.empty()) std::printf("mkvinfo not available - its checks are skipped\n");
    if (c.tools.ffmpeg.empty()) std::printf("ffmpeg not available - the decoding checks are skipped\n");
    if (c.tools.ffprobe.empty()) std::printf("ffprobe not available - the tag / chapter checks are skipped\n");

    c.dir = fs::temp_directory_path() / "cdreader_mka_check";
    fs::create_directories(c.dir);
    const std::vector<std::string> formats = cdr::audioFormats();
    for (const char* format : {"mka", "mka-pcm", "mka-opus", "mka-vorbis"}) {
        if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
            std::printf("%s: not built in - skipped\n", format);
            continue;
        }
        // A single Opus packet (under 20 ms) is left out: FFmpeg then applies
        // the end trimming but not the codec delay of that same packet.
        for (size_t samples : {size_t(1), size_t(1000), size_t(44100 * 4 + 5)}) {
            if (samples == 1 && std::string(format) == "mka-opus") continue;
            checkTrack(c, format, samples);
        }
    }
    checkImage(c, "mka");
    if (std::find(formats.begin(), formats.end(), "mka-opus") != formats.end()) checkImage(c, "mka-opus");
    fs::remove_all(c.dir);
    std::printf("\n%s\n", c.failures ? "FAILED" : "PASSED");
    return c.failures ? 1 : 0;
}
