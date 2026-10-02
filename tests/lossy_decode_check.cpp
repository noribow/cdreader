// Decodes OpusWriter / VorbisWriter output with the reference tools:
//   opusinfo / ogginfo  must accept the stream without warnings,
//   opusdec / oggdec    must decode it to exactly the input length (pre-skip
//                       and end trimming), close to the input signal,
//   ffprobe (optional)  must read the tags.
//
// Usage: cdreader_lossy_decode_check [--opusdec P] [--opusinfo P] [--oggdec P]
//                                    [--ogginfo P] [--ffprobe P]
// Exits with 77 (reported as "skipped" by ctest) when no codec can be checked.

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
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/resampler.h"
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
    m.isrc = "JPVI09912345";
    m.mcn = "4988001234567";
    return m;
}

void encode(const std::string& format, const cdr::EncoderSettings& settings, const fs::path& path,
            const std::vector<uint8_t>& pcm) {
    std::unique_ptr<cdr::AudioWriter> writer = cdr::createAudioWriter(format, settings);
    writer->open(path, metadata());
    for (size_t pos = 0; pos < pcm.size(); pos += 2352 * 13)
        writer->write(pcm.data() + pos, std::min<size_t>(2352 * 13, pcm.size() - pos));
    writer->close();
}

struct Tools {
    std::string opusdec, opusinfo, oggdec, ogginfo, ffprobe;
};

struct Checker {
    fs::path dir;
    int failures = 0;

    void expect(bool ok, const std::string& what) {
        std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
        if (!ok) ++failures;
    }

    // Runs an info tool; it must succeed and report no warning or error.
    void info(const std::string& tool, const fs::path& file, const std::string& label) {
        const fs::path out = dir / "info.txt";
        const int status = run(quote(fs::u8path(tool)) + " " + quote(file) + " > " + quote(out) + " 2>&1");
        const std::string text = lower(readText(out));
        const bool clean = text.find("warning") == std::string::npos && text.find("error") == std::string::npos;
        expect(status == 0 && clean, label);
        if (status != 0 || !clean) std::printf("%s\n", readText(out).c_str());
    }

    // ffprobe must read the tags back (keys case-insensitive).
    void tags(const std::string& ffprobe, const fs::path& file, const std::string& label) {
        const fs::path out = dir / "tags.txt";
        const int status = run(quote(fs::u8path(ffprobe)) + " -v error -show_entries stream_tags:format_tags -of "
                                                            "default=noprint_wrappers=1 " +
                               quote(file) + " > " + quote(out) + " 2>&1");
        const std::string text = lower(readText(out));
        const cdr::TrackMetadata m = metadata();
        // FFmpeg reports ALBUMARTIST as album_artist and TRACKNUMBER as track.
        const std::map<std::string, std::string> want = {
            {"title", m.title}, {"artist", m.artist}, {"album", m.album}, {"album_artist", m.albumArtist},
            {"track", "3"}, {"tracktotal", "12"}, {"date", "1999"}, {"genre", "rock"}, {"cddb", "0a0b0c03"},
            {"isrc", "jpvi09912345"}, {"barcode", "4988001234567"}};
        bool ok = status == 0;
        for (const auto& [key, value] : want) {
            const bool found = text.find("tag:" + key + "=" + lower(value) + "\n") != std::string::npos;
            if (!found) std::printf("  missing tag %s=%s\n", key.c_str(), value.c_str());
            ok = ok && found;
        }
        expect(ok, label);
        if (!ok) std::printf("%s\n", readText(out).c_str());
    }
};

void checkOpus(Checker& c, const Tools& tools) {
    const cdr::Resampler resampler(44100, 48000, 2);
    for (size_t samples : {size_t(0), size_t(1), size_t(1000), size_t(44100 * 4 + 5)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        for (int kbps : {64, 160}) {
            if (samples < 44100 && kbps != 160) continue;
            cdr::EncoderSettings settings;
            settings.bitrateKbps = kbps;
            const fs::path file = c.dir / "test.opus";
            const fs::path raw = c.dir / "decoded.raw";
            encode("opus", settings, file, s.pcm);
            const std::string label = "opus " + std::to_string(kbps) + " kbit/s, " + std::to_string(samples) + " samples";
            if (!tools.opusinfo.empty()) c.info(tools.opusinfo, file, "opusinfo: " + label);
            if (!tools.opusdec.empty()) {
                // At 48 kHz, the rate Opus runs at: exactly the resampled length.
                fs::remove(raw);
                const int status = run(quote(fs::u8path(tools.opusdec)) + " --quiet --rate 48000 --no-dither " +
                                       quote(file) + " " + quote(raw));
                const std::vector<uint8_t> pcm = readFile(raw);
                const size_t expected = size_t(resampler.outputLength(samples));
                c.expect(status == 0 && pcm.size() == expected * 4,
                         "opusdec 48 kHz: " + label + ": " + std::to_string(pcm.size() / 4) + " samples (expected " +
                             std::to_string(expected) + ")");
                if (samples > 44100 && pcm.size() == expected * 4) {
                    std::vector<double> ref, got;
                    for (size_t i = 2400; i + 2400 < expected; ++i) {
                        const double t = double(i) / 48000;
                        ref.push_back(testsig::tones(t, 0));
                        got.push_back(sampleAt(pcm, 2 * i) / 32768.0);
                        ref.push_back(testsig::tones(t, 1));
                        got.push_back(sampleAt(pcm, 2 * i + 1) / 32768.0);
                    }
                    const double snr = snrDb(ref, got);
                    char text[160];
                    std::snprintf(text, sizeof text, "opusdec 48 kHz: %s: SNR %.1f dB against the exact signal, %ju bytes",
                                  label.c_str(), snr, uintmax_t(fs::file_size(file)));
                    c.expect(snr > (kbps >= 160 ? 20 : 12), text);
                }
                // At the original rate from OpusHead (opusdec resamples back to 44.1 kHz).
                fs::remove(raw);
                const int status44 = run(quote(fs::u8path(tools.opusdec)) + " --quiet --rate 44100 --no-dither " +
                                         quote(file) + " " + quote(raw));
                const std::vector<uint8_t> pcm44 = readFile(raw);
                c.expect(status44 == 0 && pcm44.size() == samples * 4,
                         "opusdec 44.1 kHz: " + label + ": " + std::to_string(pcm44.size() / 4) + " samples (expected " +
                             std::to_string(samples) + ")");
                if (samples > 44100 && pcm44.size() == samples * 4) {
                    std::vector<double> ref, got;
                    for (size_t i = 2205; i + 2205 < samples * 2; ++i) {
                        ref.push_back(sampleAt(s.pcm, i) / 32768.0);
                        got.push_back(sampleAt(pcm44, i) / 32768.0);
                    }
                    char text[160];
                    const double snr = snrDb(ref, got);
                    std::snprintf(text, sizeof text, "opusdec 44.1 kHz: %s: SNR %.1f dB against the input", label.c_str(), snr);
                    c.expect(snr > (kbps >= 160 ? 20 : 12), text);
                }
            }
            if (!tools.ffprobe.empty() && samples > 44100 && kbps == 160) c.tags(tools.ffprobe, file, "ffprobe tags: " + label);
            fs::remove(file);
            fs::remove(raw);
        }
    }
}

void checkVorbis(Checker& c, const Tools& tools) {
    for (size_t samples : {size_t(0), size_t(1), size_t(1000), size_t(44100 * 4 + 5)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        std::vector<std::pair<std::string, cdr::EncoderSettings>> modes(1);
        modes[0].first = "quality 5 (default)";
        if (samples > 44100) {
            cdr::EncoderSettings q1, b128;
            q1.quality = 1;
            b128.bitrateKbps = 128;
            modes.push_back({"quality 1", q1});
            modes.push_back({"128 kbit/s", b128});
        }
        for (const auto& [name, settings] : modes) {
            const fs::path file = c.dir / "test.ogg";
            const fs::path raw = c.dir / "decoded.raw";
            encode("vorbis", settings, file, s.pcm);
            const std::string label = "vorbis " + name + ", " + std::to_string(samples) + " samples";
            // An empty stream necessarily has an audio packet at granule
            // position 0, which ogginfo reports as suspicious.
            if (!tools.ogginfo.empty() && samples > 0) c.info(tools.ogginfo, file, "ogginfo: " + label);
            if (!tools.oggdec.empty()) {
                fs::remove(raw);
                const int status = run(quote(fs::u8path(tools.oggdec)) + " --quiet -R -b 16 -e 0 -s 1 -o " + quote(raw) +
                                       " " + quote(file));
                const std::vector<uint8_t> pcm = readFile(raw);
                c.expect(status == 0 && pcm.size() == samples * 4,
                         "oggdec: " + label + ": " + std::to_string(pcm.size() / 4) + " samples (expected " +
                             std::to_string(samples) + ")");
                if (samples > 44100 && pcm.size() == samples * 4) {
                    std::vector<double> ref, got;
                    for (size_t i = 0; i < samples * 2; ++i) {
                        ref.push_back(sampleAt(s.pcm, i) / 32768.0);
                        got.push_back(sampleAt(pcm, i) / 32768.0);
                    }
                    const double snr = snrDb(ref, got);
                    char text[160];
                    std::snprintf(text, sizeof text, "oggdec: %s: SNR %.1f dB against the input, %ju bytes", label.c_str(),
                                  snr, uintmax_t(fs::file_size(file)));
                    c.expect(snr > 12, text);
                }
            }
            if (!tools.ffprobe.empty() && samples > 44100 && name == modes[0].first)
                c.tags(tools.ffprobe, file, "ffprobe tags: " + label);
            fs::remove(file);
            fs::remove(raw);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Tools tools;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--opusdec") tools.opusdec = value;
        else if (option == "--opusinfo") tools.opusinfo = value;
        else if (option == "--oggdec") tools.oggdec = value;
        else if (option == "--ogginfo") tools.ogginfo = value;
        else if (option == "--ffprobe") tools.ffprobe = value;
    }
    const std::vector<std::string> formats = cdr::audioFormats();
    const bool haveOpus = std::find(formats.begin(), formats.end(), "opus") != formats.end();
    const bool haveVorbis = std::find(formats.begin(), formats.end(), "vorbis") != formats.end();
    const bool opus = haveOpus && (!tools.opusdec.empty() || !tools.opusinfo.empty());
    const bool vorbis = haveVorbis && (!tools.oggdec.empty() || !tools.ogginfo.empty());
    if (!opus && !vorbis) {
        std::printf("no lossy format with a reference decoder available (opus-tools / vorbis-tools) - skipping\n");
        return 77;
    }

    Checker c;
    c.dir = fs::temp_directory_path() / "cdreader_lossy_decode_check";
    fs::create_directories(c.dir);
    if (opus) checkOpus(c, tools);
    else std::printf("Opus: %s - skipped\n", haveOpus ? "opusdec / opusinfo not available" : "not built in");
    if (vorbis) checkVorbis(c, tools);
    else std::printf("Vorbis: %s - skipped\n", haveVorbis ? "oggdec / ogginfo not available" : "not built in");
    fs::remove_all(c.dir);
    std::printf("\n%s\n", c.failures ? "FAILED" : "PASSED");
    return c.failures ? 1 : 0;
}
