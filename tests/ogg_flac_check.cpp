// Ogg FLAC output (OggFlacWriter) checked with the reference tools:
//   flac      `flac -t --ogg` (CRCs + MD5) and `flac -d --ogg` must give the
//             input PCM bit for bit; for a disc image, `flac -d --cue=` (which
//             reads the CUESHEET block through libFLAC) must cut each track at
//             its position (and at the INDEX 00 / 02 points of #25),
//   ogginfo   must accept the stream without warnings,
//   ffmpeg    must decode it to the input PCM bit for bit,
//   ffprobe   must read the tags (and the CUESHEET tag of a disc image).
// metaflac only reads native FLAC files, so it is not used here.
//
// Usage: cdreader_ogg_flac_check [--flac P] [--ogginfo P] [--ffmpeg P] [--ffprobe P]
// Exits with 77 (reported as "skipped" by ctest) when no tool is given.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/ogg_flac_writer.h"
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

void encode(const fs::path& path, const std::vector<uint8_t>& pcm, const cdr::TrackMetadata& meta,
            const cdr::EmbeddedCueSheet* cue = nullptr) {
    cdr::OggFlacWriter writer;
    if (cue) writer.setEmbeddedCueSheet(*cue);
    writer.open(path, meta);
    size_t pos = 0;
    if (pcm.size() > 7) {  // an odd split to exercise partial samples
        writer.write(pcm.data(), 7);
        pos = 7;
    }
    for (; pos < pcm.size(); pos += 2352) writer.write(pcm.data() + pos, std::min<size_t>(2352, pcm.size() - pos));
    writer.close();
}

struct Tools {
    std::string flac, ogginfo, ffmpeg, ffprobe;
};

struct Checker {
    Tools tools;
    fs::path dir;
    int failures = 0;

    void expect(bool ok, const std::string& what) {
        std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
        if (!ok) ++failures;
    }

    std::string flacDecode(const fs::path& file, const fs::path& raw, const std::string& extra = {}) {
        return quote(fs::u8path(tools.flac)) + " -s -d -f --ogg " + extra +
               " --force-raw-format --endian=little --sign=signed -o " + quote(raw) + " " + quote(file);
    }

    // `flac -t` (frame CRCs and the MD5 of STREAMINFO) and `flac -d`.
    void flac(const fs::path& file, const std::vector<uint8_t>& pcm, const std::string& label) {
        expect(run(quote(fs::u8path(tools.flac)) + " -s -t --ogg " + quote(file)) == 0, "flac -t: " + label);
        const fs::path raw = dir / "flac.raw";
        fs::remove(raw);
        const int status = run(flacDecode(file, raw));
        expect(status == 0 && readFile(raw) == pcm, "flac -d: bit-identical PCM: " + label);
        fs::remove(raw);
    }

    void ogginfo(const fs::path& file, const std::string& label) {
        const fs::path out = dir / "info.txt";
        const int status = run(quote(fs::u8path(tools.ogginfo)) + " " + quote(file) + " > " + quote(out) + " 2>&1");
        const std::string text = lower(readText(out));
        const bool clean = text.find("warning") == std::string::npos && text.find("error") == std::string::npos &&
                           text.find("type flac") != std::string::npos;
        expect(status == 0 && clean, "ogginfo: " + label);
        if (status != 0 || !clean) std::printf("%s\n", readText(out).c_str());
    }

    void ffmpeg(const fs::path& file, const std::vector<uint8_t>& pcm, const std::string& label) {
        const fs::path raw = dir / "ffmpeg.raw";
        fs::remove(raw);
        const int status = run(quote(fs::u8path(tools.ffmpeg)) + " -v error -nostdin -y -i " + quote(file) +
                               " -f s16le -acodec pcm_s16le " + quote(raw));
        expect(status == 0 && readFile(raw) == pcm, "ffmpeg: bit-identical PCM: " + label);
        fs::remove(raw);
    }

    // ffprobe must read the tags back (keys case-insensitive); `contains`:
    // further text expected in the output (lines of a multi-line tag).
    void tags(const fs::path& file, const std::map<std::string, std::string>& want, const std::string& label,
              const std::vector<std::string>& contains = {}) {
        const fs::path out = dir / "tags.txt";
        const int status = run(quote(fs::u8path(tools.ffprobe)) +
                               " -v error -show_entries stream=codec_name:stream_tags:format_tags -of "
                               "default=noprint_wrappers=1 " +
                               quote(file) + " > " + quote(out) + " 2>&1");
        const std::string text = lower(readText(out));
        bool ok = status == 0 && text.find("codec_name=flac\n") != std::string::npos;
        for (const auto& [key, value] : want) {
            const bool found = text.find("tag:" + key + "=" + lower(value) + "\n") != std::string::npos;
            if (!found) std::printf("  missing tag %s=%s\n", key.c_str(), value.c_str());
            ok = ok && found;
        }
        for (const std::string& t : contains) {
            const bool found = text.find(lower(t)) != std::string::npos;
            if (!found) std::printf("  missing text %s\n", t.c_str());
            ok = ok && found;
        }
        expect(ok, "ffprobe: " + label);
        if (!ok) std::printf("%s\n", readText(out).c_str());
    }
};

void checkSignals(Checker& c) {
    std::vector<testsig::Signal> signals = testsig::all();
    signals.push_back(testsig::music(0));
    signals.back().name = "empty";
    signals.push_back(testsig::music(4096));
    signals.back().name = "one full block";
    for (const testsig::Signal& s : signals) {
        const fs::path file = c.dir / "test.oga";
        encode(file, s.pcm, metadata());
        const std::string label = s.name + " (" + std::to_string(s.pcm.size() / 4) + " samples)";
        if (!c.tools.flac.empty()) c.flac(file, s.pcm, label);
        // A stream without audio (headers only, not something a CD track
        // produces) is reported by ogginfo ("did not contain data packets")
        // and cannot be opened by FFmpeg's Ogg demuxer; flac handles it.
        if (!c.tools.ogginfo.empty() && !s.pcm.empty()) c.ogginfo(file, label);
        if (!c.tools.ffmpeg.empty() && !s.pcm.empty()) c.ffmpeg(file, s.pcm, label);
        fs::remove(file);
    }
    if (!c.tools.ffprobe.empty()) {
        const fs::path file = c.dir / "tags.oga";
        encode(file, testsig::music(44100).pcm, metadata());
        const cdr::TrackMetadata m = metadata();
        // FFmpeg reports ALBUMARTIST as album_artist and TRACKNUMBER as track.
        c.tags(file,
               {{"title", m.title}, {"artist", m.artist}, {"album", m.album}, {"album_artist", m.albumArtist},
                {"track", "3"}, {"tracktotal", "12"}, {"date", "1999"}, {"genre", "rock"}, {"cddb", "0a0b0c03"}},
               "tags");
        fs::remove(file);
    }
}

// A three-track disc image with an embedded CUE sheet. With `gaps` (#25):
// an HTOA of 10 sectors (track 1: INDEX 00 at 0, INDEX 01 at 10), a pregap
// of 10 sectors before track 2 and an INDEX 02 in track 3; `flac -d --cue=`
// must find every index point in the CUESHEET block.
void checkImage(Checker& c, bool gaps) {
    const uint32_t starts[] = {gaps ? 10u : 0u, 40, 115};  // INDEX 01, sectors
    const uint32_t sectors = 200;
    const testsig::Signal s = testsig::music(size_t(sectors) * cdr::kSamplesPerSector);
    cdr::AlbumMetadata album;
    album.title = "Album";
    album.artist = "Artist";
    cdr::EmbeddedCueSheet cue;
    for (int i = 0; i < 3; ++i) {
        cdr::CueTrack t;
        t.number = i + 1;
        t.file = "Image.oga";
        t.startSectors = starts[i];
        t.title = "Track " + std::to_string(i + 1);
        cue.tracks.push_back(t);
    }
    if (gaps) {
        cue.tracks[0].hasIndex00 = true;  // the HTOA
        cue.tracks[1].hasIndex00 = true;
        cue.tracks[1].index00Sectors = 30;
        cue.tracks[2].laterIndexes = {150};
    }
    cue.totalSectors = sectors;
    cue.text = cdr::formatCueSheet(album, cue.tracks);
    const fs::path file = c.dir / "image.oga";
    encode(file, s.pcm, album.forTrack(0, 3), &cue);
    const std::string label = gaps ? "disc image with HTOA / pregap CUESHEET" : "disc image with CUESHEET";
    auto cut = [&](const std::string& range, uint32_t from, uint32_t to, const std::string& what) {
        const fs::path raw = c.dir / "track.raw";
        fs::remove(raw);
        const int status = run(c.flacDecode(file, raw, "--cue=" + range));
        const std::vector<uint8_t> want(s.pcm.begin() + std::ptrdiff_t(size_t(from) * cdr::kSectorBytes),
                                        s.pcm.begin() + std::ptrdiff_t(size_t(to) * cdr::kSectorBytes));
        c.expect(status == 0 && readFile(raw) == want, "flac -d --cue=" + range + ": " + what + " from the CUESHEET block");
        fs::remove(raw);
    };
    if (!c.tools.flac.empty()) {
        c.flac(file, s.pcm, label);
        // Track n = INDEX 01 of track n up to INDEX 01 of track n + 1 (the last one to the end).
        for (int i = 0; i < 3; ++i) {
            const std::string range = std::to_string(i + 1) + ".1" + (i < 2 ? "-" + std::to_string(i + 2) + ".1" : "");
            cut(range, starts[i], i < 2 ? starts[i + 1] : sectors, "track " + std::to_string(i + 1));
        }
        if (gaps) {
            cut("1.0-1.1", 0, 10, "the HTOA");
            cut("2.0-2.1", 30, 40, "the pregap of track 2");
            cut("3.2", 150, sectors, "INDEX 02 of track 3");
        }
    }
    if (!c.tools.ogginfo.empty()) c.ogginfo(file, label);
    if (!c.tools.ffmpeg.empty()) c.ffmpeg(file, s.pcm, label);
    if (!c.tools.ffprobe.empty()) {
        // The CUESHEET tag (multi-line text) with each track's index points.
        if (gaps)
            c.tags(file, {{"album", "Album"}}, label + ": tags and CUESHEET tag",
                   {"tag:cuesheet=", "INDEX 00 00:00:00", "INDEX 01 00:00:10", "INDEX 00 00:00:30", "INDEX 02 00:02:00"});
        else
            c.tags(file, {{"album", "Album"}}, label + ": tags and CUESHEET tag",
                   {"tag:cuesheet=", "TRACK 01 AUDIO", "INDEX 01 00:00:40", "TRACK 03 AUDIO", "INDEX 01 00:01:40"});
    }
    fs::remove(file);
}

}  // namespace

int main(int argc, char** argv) {
    Checker c;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        const std::string value = argv[i + 1];
        if (option == "--flac") c.tools.flac = value;
        else if (option == "--ogginfo") c.tools.ogginfo = value;
        else if (option == "--ffmpeg") c.tools.ffmpeg = value;
        else if (option == "--ffprobe") c.tools.ffprobe = value;
    }
    if (c.tools.flac.empty() && c.tools.ogginfo.empty() && c.tools.ffmpeg.empty() && c.tools.ffprobe.empty()) {
        std::printf("no reference tool available (flac / ogginfo / ffmpeg / ffprobe) - skipping\n");
        return 77;
    }
    for (const auto& [name, path] : {std::pair<const char*, const std::string&>{"flac", c.tools.flac},
                                     {"ogginfo", c.tools.ogginfo},
                                     {"ffmpeg", c.tools.ffmpeg},
                                     {"ffprobe", c.tools.ffprobe}})
        if (path.empty()) std::printf("%s not available - its checks are skipped\n", name);

    c.dir = fs::temp_directory_path() / "cdreader_ogg_flac_check";
    fs::create_directories(c.dir);
    checkSignals(c);
    checkImage(c, false);
    checkImage(c, true);
    fs::remove_all(c.dir);
    std::printf("\n%s\n", c.failures ? "FAILED" : "PASSED");
    return c.failures ? 1 : 0;
}
