// Tests for the Android ripping workflow (platform/android/rip_session.h):
// a fake drive behind the fake USB bridge and the Bulk-Only Transport, with
// canned CDDB and AccurateRip responses. Same minimal runner as test_main.cpp.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "alac_decoder.h"
#include "bot_transport.h"
#include "cdreader/accuraterip.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/http.h"
#include "cdreader/ripper.h"
#include "fake_drive.h"
#include "fake_usb_device.h"
#include "flac_decoder.h"
#include "rip_session.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define TEST(name)                                  \
    static void name();                             \
    static Register reg_##name(#name, name);        \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        auto va = (a);                                                                          \
        auto vb = (b);                                                                          \
        if (!(va == vb)) {                                                                      \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__, \
                         __LINE__, #a, #b, (long long)va, (long long)vb);                       \
            ++failures;                                                                         \
        }                                                                                       \
    } while (0)

#define CHECK_STR(a, b)                                                                                     \
    do {                                                                                                    \
        const std::string va = (a);                                                                         \
        const std::string vb = (b);                                                                         \
        if (va != vb) {                                                                                     \
            std::fprintf(stderr, "  %s:%d: CHECK_STR(%s, %s) failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                         #a, #b, va.c_str(), vb.c_str());                                                   \
            ++failures;                                                                                     \
        }                                                                                                   \
    } while (0)

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Three audio tracks; lead-out at 10 seconds (as in test_main.cpp).
FakeDrive makeAudioDisc() { return FakeDrive({{0, false}, {300, false}, {450, false}}, 750); }

// A fake drive behind a fake USB bridge behind the BOT, as in the app.
struct Rig {
    FakeDrive fake = makeAudioDisc();
    FakeUsbDevice device{fake, 0};
    cdr::usb::BulkOnlyTransport bot{device, 0};
    cdr::CdDrive drive{bot};
    cdr::RipSession session{drive};
};

// The reference: a track ripped straight from a FakeDrive with the core ripper.
struct Reference {
    std::vector<uint8_t> pcm;
    uint32_t crc32 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
};

Reference referenceRip(int number, int offset = 0) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::RipOptions options;
    options.readOffsetSamples = offset;
    cdr::Ripper ripper(drive, toc, options);
    const cdr::Track& track = *toc.findTrack(number);
    cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, track);
    Reference ref;
    ref.crc32 = ripper
                    .ripTrack(track,
                              [&](const uint8_t* p, size_t n) {
                                  ref.pcm.insert(ref.pcm.end(), p, p + n);
                                  ar.update(p, n);
                              })
                    .crc32;
    ref.v1 = ar.v1();
    ref.v2 = ar.v2();
    return ref;
}

cdr::HttpResponse reply(int status, const std::string& body) {
    cdr::HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = body;
    return r;
}

// CDDB answers keyed by command prefix ("cddb query", "cddb read rock"), one
// answer for every AccurateRip URL; records every URL requested.
struct FakeHttp : cdr::HttpClient {
    std::map<std::string, cdr::HttpResponse> cddb;
    cdr::HttpResponse accurateRip;
    std::vector<std::string> urls;

    FakeHttp() { accurateRip.error = "no route"; }

    cdr::HttpResponse get(const std::string& url) override {
        urls.push_back(url);
        if (contains(url, "accuraterip.com")) return accurateRip;
        const size_t cmd = url.find("cmd=");
        if (cmd != std::string::npos) {
            const size_t end = url.find('&', cmd);
            const std::string encoded = url.substr(cmd + 4, end - cmd - 4);
            for (const auto& [prefix, response] : cddb)
                if (encoded.rfind(cdr::urlEncode(prefix), 0) == 0) return response;
        }
        cdr::HttpResponse r;
        r.error = "no route";
        return r;
    }

    int accurateRipRequests() const {
        return int(std::count_if(urls.begin(), urls.end(), [](const std::string& u) { return contains(u, "accuraterip.com"); }));
    }
};

// One exact match (a compilation: every track has its own artist) whose
// titles need file name sanitizing.
void addCddbAlbum(FakeHttp& http) {
    http.cddb["cddb query"] = reply(200, "200 misc 1c01ce03 Various / Album: Live\r\n");
    http.cddb["cddb read misc"] = reply(200,
                                        "210 misc 1c01ce03\r\n"
                                        "DTITLE=Various / Album: Live\r\n"
                                        "DYEAR=1999\r\n"
                                        "DGENRE=Rock\r\n"
                                        "TTITLE0=The Artist / Opening\r\n"
                                        "TTITLE1=Guest / \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91\r\n"  // 夜明け
                                        "TTITLE2=The Artist / What?\r\n"
                                        ".\r\n");
}

// A dBAR file: one record per pressing, entries (confidence, checksum) in TOC order.
std::string dbar(const cdr::AccurateRipDiscId& id,
                 const std::vector<std::vector<std::pair<uint8_t, uint32_t>>>& pressings) {
    std::string out;
    auto le32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(char(uint8_t(v >> (8 * i))));
    };
    for (const auto& entries : pressings) {
        out.push_back(char(uint8_t(id.audioTracks)));
        le32(id.id1);
        le32(id.id2);
        le32(id.cddb);
        for (const auto& [confidence, checksum] : entries) {
            out.push_back(char(confidence));
            le32(checksum);
            le32(0);  // frame 450 checksum (offset detection only)
        }
    }
    return out;
}

// A scratch directory removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("cdreader_rip_session_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool hasComment(const DecodedFlac& d, const std::string& comment) {
    return std::find(d.comments.begin(), d.comments.end(), comment) != d.comments.end();
}

cdr::RipSettings settings(const std::string& format, int offset = 0) {
    cdr::RipSettings s;
    s.format = format;
    s.options.readOffsetSamples = offset;
    s.options.maxRetries = 2;
    return s;
}

}  // namespace

TEST(reads_drive_and_toc_through_usb) {
    Rig rig;
    CHECK(rig.session.driveName().rfind("FAKE", 0) == 0);
    const cdr::Toc& toc = rig.session.toc();
    CHECK_EQ(toc.tracks.size(), size_t(3));
    CHECK_EQ(toc.leadOutLba, 750u);
    // Without metadata: generic names from the disc id.
    char id[16];
    std::snprintf(id, sizeof id, "%08X", toc.cddbId());
    CHECK_STR(rig.session.album().discId, id);
    CHECK_STR(rig.session.albumDirectoryName(), std::string("cd_") + id);
    CHECK_STR(rig.session.trackFileName(2, "flac"), "Track02.flac");
    CHECK_STR(rig.session.trackFileName(2, "wav"), "Track02.wav");
    CHECK(rig.session.trackLabel(1).empty());
}

TEST(cddb_metadata_names_files_and_folder) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    cdr::CddbSettings cddb;
    cddb.options.server = "https://cddb.example/cgi";
    const cdr::CddbLookupResult& r = rig.session.lookupCddb(&http, cddb);
    CHECK(r.found && r.exact);
    CHECK_EQ(http.urls.size(), size_t(2));
    CHECK(!http.urls.empty() && http.urls[0].rfind("https://cddb.example/cgi?cmd=cddb+query+", 0) == 0);
    CHECK_STR(rig.session.album().artist, "Various");
    CHECK_STR(rig.session.album().title, "Album: Live");
    CHECK_STR(rig.session.albumDirectoryName(), "Various - Album_ Live");
    CHECK_STR(rig.session.trackFileName(1, "flac"), "01 - Opening.flac");
    CHECK_STR(rig.session.trackFileName(2, "wav"), "02 - \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91.wav");
    CHECK_STR(rig.session.trackFileName(3, "flac"), "03 - What_.flac");
    CHECK_STR(rig.session.trackLabel(1), "The Artist / Opening");
    CHECK_STR(rig.session.trackLabel(2), "Guest / \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91");
    bool threw = false;
    try {
        rig.session.trackFileName(1, "mp3");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Turning the lookup off goes back to generic names without a request.
    cddb.enabled = false;
    rig.session.lookupCddb(&http, cddb);
    CHECK_EQ(http.urls.size(), size_t(2));
    CHECK_STR(rig.session.trackFileName(1, "flac"), "Track01.flac");
    CHECK(rig.session.albumDirectoryName().rfind("cd_", 0) == 0);

    // A new TOC forgets the metadata of the previous disc.
    cddb.enabled = true;
    rig.session.lookupCddb(&http, cddb);
    CHECK_STR(rig.session.trackFileName(1, "flac"), "01 - Opening.flac");
    rig.session.readToc();
    CHECK_STR(rig.session.trackFileName(1, "flac"), "Track01.flac");
}

TEST(cddb_inexact_matches_pick_by_index) {
    Rig rig;
    FakeHttp http;
    http.cddb["cddb query"] = reply(200,
                                    "211 Found inexact matches, list follows (until terminating `.')\r\n"
                                    "rock 1c01ce03 First / One\r\nmisc 1c01ce04 Second / Two\r\n.\r\n");
    http.cddb["cddb read rock"] = reply(200, "210 rock 1c01ce03\r\nDTITLE=First / One\r\nTTITLE0=A\r\n.\r\n");
    http.cddb["cddb read misc"] = reply(200, "210 misc 1c01ce04\r\nDTITLE=Second / Two\r\nTTITLE0=B\r\n.\r\n");
    cdr::CddbSettings cddb;
    cddb.options.matchIndex = 1;
    const cdr::CddbLookupResult& r = rig.session.lookupCddb(&http, cddb);
    CHECK(r.found && !r.exact);
    CHECK_EQ(r.matches.size(), size_t(2));
    CHECK_EQ(r.chosen, size_t(1));
    CHECK_STR(rig.session.albumDirectoryName(), "Second - Two");
    CHECK_STR(rig.session.trackFileName(1, "wav"), "01 - B.wav");
    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "2 inexact match(es)"));
    CHECK(contains(log, "  * 2. misc/1c01ce04  Second / Two"));
}

TEST(flac_rip_carries_cddb_tags_and_exact_audio) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    rig.session.beginRip(settings("flac"));
    TempDir dir;
    for (int n = 1; n <= 3; ++n) {
        const fs::path path = dir.path / ("track" + std::to_string(n) + ".flac");
        const cdr::RippedTrack& r = rig.session.ripTrack(n, path);
        const Reference ref = referenceRip(n);
        CHECK_EQ(r.track.number, n);
        CHECK(r.result.clean());
        CHECK_EQ(r.result.crc32, ref.crc32);
        CHECK_EQ(r.accurateRipV1, ref.v1);
        CHECK_EQ(r.accurateRipV2, ref.v2);
        CHECK_STR(r.fileName, rig.session.trackFileName(n, "flac"));

        // STREAMINFO is patched at close: total samples and MD5 are right.
        const DecodedFlac d = decodeFlac(readFile(path));
        CHECK_EQ(d.totalSamples, uint64_t(ref.pcm.size() / 4));
        CHECK(d.pcm == ref.pcm);
        CHECK(hasComment(d, "ALBUM=Album: Live"));
        CHECK(hasComment(d, "ALBUMARTIST=Various"));
        CHECK(hasComment(d, "TRACKNUMBER=" + std::to_string(n)));
        CHECK(hasComment(d, "TRACKTOTAL=3"));
        CHECK(hasComment(d, "DATE=1999"));
        CHECK(hasComment(d, "GENRE=Rock"));
        CHECK(hasComment(d, "CDDB=" + rig.session.album().discId));
    }
    const DecodedFlac d1 = decodeFlac(readFile(dir.path / "track1.flac"));
    CHECK(hasComment(d1, "TITLE=Opening"));
    CHECK(hasComment(d1, "ARTIST=The Artist"));
    const DecodedFlac d2 = decodeFlac(readFile(dir.path / "track2.flac"));
    CHECK(hasComment(d2, "TITLE=\xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91"));
    CHECK(hasComment(d2, "ARTIST=Guest"));
    CHECK_EQ(rig.session.rippedTracks().size(), size_t(3));
    CHECK_EQ(rig.session.problemTracks(), 0);
}

// Packets of an Ogg FLAC file put back together as a native FLAC stream:
// "fLaC" + the metadata blocks (the first packet's from offset 13) + the frames.
std::vector<uint8_t> oggFlacAsNative(const std::vector<uint8_t>& ogg) {
    std::vector<std::vector<uint8_t>> packets(1);
    for (size_t pos = 0; pos + 27 <= ogg.size();) {
        if (std::memcmp(&ogg[pos], "OggS", 4) != 0) throw std::runtime_error("bad Ogg page");
        const size_t segments = ogg[pos + 26];
        size_t body = pos + 27 + segments;
        for (size_t i = 0; i < segments; ++i) {
            const size_t length = ogg[pos + 27 + i];
            packets.back().insert(packets.back().end(), ogg.begin() + long(body), ogg.begin() + long(body + length));
            body += length;
            if (length < 255) packets.emplace_back();
        }
        pos = body;
    }
    packets.pop_back();
    if (packets.empty() || packets[0].size() != 51 || packets[0][0] != 0x7F) throw std::runtime_error("not Ogg FLAC");
    std::vector<uint8_t> native(packets[0].begin() + 9, packets[0].end());
    for (size_t i = 1; i < packets.size(); ++i) native.insert(native.end(), packets[i].begin(), packets[i].end());
    return native;
}

TEST(oggflac_rip_carries_tags_and_exact_audio) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    rig.session.beginRip(settings("oggflac"));
    TempDir dir;
    const fs::path path = dir.path / "track";
    const cdr::RippedTrack& r = rig.session.ripTrack(2, path);
    const Reference ref = referenceRip(2);
    CHECK_EQ(r.result.crc32, ref.crc32);
    CHECK_EQ(r.accurateRipV2, ref.v2);
    CHECK_STR(r.fileName, "02 - \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91.oga");
    CHECK_STR(r.fileName, rig.session.trackFileName(2, "oggflac"));
    // STREAMINFO is patched at close (the first page rewritten): the total is right.
    const DecodedFlac d = decodeFlac(oggFlacAsNative(readFile(path)));
    CHECK_EQ(d.totalSamples, uint64_t(ref.pcm.size() / 4));
    CHECK(d.pcm == ref.pcm);
    CHECK(hasComment(d, "TITLE=\xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91"));
    CHECK(hasComment(d, "ARTIST=Guest"));
    CHECK(hasComment(d, "TRACKNUMBER=2"));
    CHECK(contains(rig.session.ripLog(), "Format: oggflac\nEncoder: Ogg FLAC (built-in encoder), lossless"));
}

TEST(mka_rip_writes_matroska_with_tags) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    rig.session.beginRip(settings("mka"));
    TempDir dir;
    const fs::path path = dir.path / "track1.mka";
    const cdr::RippedTrack& r = rig.session.ripTrack(1, path);
    CHECK(r.result.clean());
    CHECK_STR(r.fileName, "01 - Opening.mka");
    const std::vector<uint8_t> bytes = readFile(path);
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.compare(0, 4, "\x1A\x45\xDF\xA3") == 0);  // EBML header
    CHECK(contains(text, "matroska"));
    CHECK(contains(text, "A_FLAC"));
    CHECK(contains(text, "Album: Live"));
    CHECK(contains(text, "Opening"));
    CHECK(contains(rig.session.ripLog(), "Format: mka\nEncoder: FLAC (built-in encoder) in Matroska"));
}

TEST(alac_rip_carries_cddb_tags_and_exact_audio) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    rig.session.beginRip(settings("alac"));
    TempDir dir;
    const fs::path path = dir.path / "track2.m4a";
    const cdr::RippedTrack& r = rig.session.ripTrack(2, path);
    const Reference ref = referenceRip(2);
    CHECK(r.result.clean());
    CHECK_EQ(r.result.crc32, ref.crc32);
    CHECK_EQ(r.accurateRipV2, ref.v2);
    CHECK_STR(r.fileName, "02 - \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91.m4a");
    CHECK_STR(r.fileName, rig.session.trackFileName(2, "alac"));

    const DecodedM4a d = decodeM4a(readFile(path));
    CHECK(d.sampleEntryType == "alac");
    CHECK_EQ(d.mediaDuration, uint64_t(ref.pcm.size() / 4));
    CHECK(d.pcm == ref.pcm);
    CHECK_STR(d.tags.at("\xA9nam"), "\xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91");
    CHECK_STR(d.tags.at("\xA9" "ART"), "Guest");
    CHECK_STR(d.tags.at("\xA9" "alb"), "Album: Live");
    CHECK_STR(d.tags.at("aART"), "Various");
    CHECK_STR(d.tags.at("trkn"), "2/3");
    CHECK_STR(d.tags.at("\xA9" "day"), "1999");
    CHECK_STR(d.tags.at("\xA9gen"), "Rock");
    CHECK_STR(d.tags.at("----:com.apple.iTunes:CDDB"), rig.session.album().discId);
    CHECK(contains(rig.session.ripLog(), "Format: alac\nEncoder: ALAC (built-in encoder), lossless"));
}

#if defined(CDREADER_HAVE_OPUS) || defined(CDREADER_HAVE_VORBIS)
TEST(lossy_rips_carry_tags_and_log_the_encoder) {
    std::vector<std::string> formats;
#ifdef CDREADER_HAVE_OPUS
    formats.push_back("opus");
#endif
#ifdef CDREADER_HAVE_VORBIS
    formats.push_back("vorbis");
#endif
    for (const std::string& format : formats) {
        Rig rig;
        FakeHttp http;
        addCddbAlbum(http);
        rig.session.lookupCddb(&http, {});
        rig.session.beginRip(settings(format));
        TempDir dir;
        const fs::path path = dir.path / "track";
        const cdr::RippedTrack& r = rig.session.ripTrack(2, path);
        const Reference ref = referenceRip(2);
        CHECK_EQ(r.result.crc32, ref.crc32);  // checksums are of the PCM read, whatever the format
        CHECK_EQ(r.accurateRipV2, ref.v2);
        const std::string ext = format == "opus" ? ".opus" : ".ogg";
        CHECK_STR(r.fileName, "02 - \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91" + ext);
        CHECK_STR(r.fileName, rig.session.trackFileName(2, format));
        const std::vector<uint8_t> file = readFile(path);
        const std::string text(file.begin(), file.end());
        CHECK(text.compare(0, 4, "OggS") == 0);
        CHECK(text.find(format == "opus" ? "OpusHead" : "\x01vorbis") != std::string::npos);
        CHECK(text.find("TITLE=\xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91") != std::string::npos);
        CHECK(text.find("ARTIST=Guest") != std::string::npos);
        CHECK(text.find("TRACKNUMBER=2") != std::string::npos);
        const std::string log = rig.session.ripLog();
        CHECK(contains(log, "Format: " + format + "\nEncoder: " + (format == "opus" ? "Opus (libopus" : "Vorbis (")));
    }
}
#endif

TEST(wav_rip_with_read_offset) {
    Rig rig;
    rig.session.beginRip(settings("wav", 6));
    TempDir dir;
    const fs::path path = dir.path / "track.wav";
    const cdr::RippedTrack& r = rig.session.ripTrack(2, path);
    const Reference ref = referenceRip(2, 6);
    CHECK_STR(r.fileName, "Track02.wav");
    CHECK_EQ(r.result.crc32, ref.crc32);
    CHECK_EQ(r.accurateRipV1, ref.v1);
    CHECK_EQ(r.accurateRipV2, ref.v2);
    CHECK(ref.pcm != referenceRip(2).pcm);  // the offset made a difference

    const std::vector<uint8_t> file = readFile(path);
    CHECK(file.size() > 44 + ref.pcm.size());
    CHECK(std::memcmp(file.data(), "RIFF", 4) == 0 && std::memcmp(file.data() + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(file.data() + 36, "data", 4) == 0);
    const uint32_t dataSize = uint32_t(file[40]) | uint32_t(file[41]) << 8 | uint32_t(file[42]) << 16 |
                              uint32_t(file[43]) << 24;
    CHECK_EQ(dataSize, uint32_t(ref.pcm.size()));
    CHECK(std::equal(ref.pcm.begin(), ref.pcm.end(), file.begin() + 44));

    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "Read offset correction: +6 samples"));
    CHECK(contains(log, "Format: wav"));
    char line[64];
    std::snprintf(line, sizeof line, "Track02.wav  CRC32 %08X  retries 0  OK", ref.crc32);
    CHECK(contains(log, line));
}

TEST(rip_settings_are_validated) {
    Rig rig;
    auto rejects = [&](const cdr::RipSettings& s) {
        try {
            rig.session.beginRip(s);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(rejects(settings("mp3")));
    CHECK(rejects(settings("wav", 100 * 588 + 1)));
    cdr::RipSettings negative = settings("wav");
    negative.options.maxRetries = -1;
    CHECK(rejects(negative));
    CHECK(!rejects(settings("flac", -100 * 588)));
    cdr::RipSettings lossless = settings("flac");
    lossless.encoder.bitrateKbps = 128;
    CHECK(rejects(lossless));  // no bitrate for lossless formats
#ifdef CDREADER_HAVE_OPUS
    cdr::RipSettings opus = settings("opus");
    opus.encoder.bitrateKbps = 1000;
    CHECK(rejects(opus));
    opus.encoder.bitrateKbps = 96;
    CHECK(!rejects(opus));
#endif

    TempDir dir;
    bool threw = false;
    try {
        rig.session.ripTrack(7, dir.path / "x.flac");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(accuraterip_reports_v1_and_v2_matches) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(rig.session.toc());
    const Reference t1 = referenceRip(1), t2 = referenceRip(2), t3 = referenceRip(3);
    CHECK(t1.v1 != t1.v2 && t2.v1 != t2.v2);
    // Pressing 1: track 1 matches v2 (12), track 2 v1 (4), track 3 nothing (5).
    // Pressing 2: track 1 nothing (3), track 2 v2 (2), track 3 only a placeholder.
    http.accurateRip = reply(200, dbar(id, {{{12, t1.v2}, {4, t2.v1}, {5, t3.v1 ^ 1}},
                                            {{3, t1.v1 ^ 0x55}, {2, t2.v2}, {0, 0}}}));

    rig.session.beginRip(settings("flac"));
    TempDir dir;
    for (int n = 1; n <= 3; ++n) rig.session.ripTrack(n, dir.path / "t.flac");
    const cdr::AccurateRipReport& report = rig.session.checkAccurateRip(&http);
    CHECK(report.status == cdr::AccurateRipReport::Status::Found);
    CHECK_EQ(http.accurateRipRequests(), 1);
    CHECK(http.urls.back() == id.url());
    CHECK_EQ(report.pressings, size_t(2));
    CHECK_EQ(report.tracks.size(), size_t(3));
    CHECK_EQ(report.accurateTracks(), 2);
    CHECK_EQ(report.tracksInDatabase(), 3);
    if (report.tracks.size() == 3) {
        const cdr::AccurateRipTrackResult& a = report.tracks[0];
        CHECK_EQ(a.track, 1);
        CHECK_STR(a.matchedVersion(), "v2");
        CHECK_EQ(a.v2Confidence, 12);
        CHECK_EQ(a.v1Confidence, 0);
        CHECK_EQ(a.totalConfidence, 15);
        CHECK_EQ(a.matchingPressings(), 1);
        CHECK_EQ(a.pressings.size(), size_t(2));
        CHECK_STR(a.describe(), "Accurately ripped with v2 (v2 12, v1 0 of 15 submissions; 1 of 2 pressings)");

        const cdr::AccurateRipTrackResult& b = report.tracks[1];
        CHECK_STR(b.matchedVersion(), "v1+v2");
        CHECK_EQ(b.v1Confidence, 4);
        CHECK_EQ(b.v2Confidence, 2);
        CHECK_EQ(b.totalConfidence, 6);
        CHECK_EQ(b.matchingPressings(), 2);

        const cdr::AccurateRipTrackResult& c = report.tracks[2];
        CHECK(!c.accurate() && c.inDatabase());
        CHECK_EQ(c.totalConfidence, 5);
        CHECK_EQ(c.pressings.size(), size_t(1));
    }

    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "AccurateRip (disc id " + id.toString() + "), 2 pressing(s) in database"));
    char line[128];
    std::snprintf(line, sizeof line, "Track 01  v1 %08X  v2 %08X  Accurately ripped with v2", t1.v1, t1.v2);
    CHECK(contains(log, line));
    std::snprintf(line, sizeof line, "          pressing 1: %08X  confidence  12  v2 match", t1.v2);
    CHECK(contains(log, line));
    CHECK(contains(log, "AccurateRip: 2 of 3 track(s) accurately ripped (v1+v2: 1, v2: 1), 3 track(s) in database"));
    // The rest of the log, as in the CLI.
    CHECK(contains(log, "Drive: FAKE"));
    CHECK(contains(log, "Format: flac"));
    CHECK(contains(log, "Mode: burst, retries 2"));
    CHECK(contains(log, "CDDB lookup (" + std::string(cdr::kDefaultCddbServer) + "): 1 exact match(es)"));
    CHECK(contains(log, "Artist: Various\nAlbum: Album: Live\nYear: 1999\nGenre: Rock"));
    CHECK(contains(log, "Folder: Various - Album_ Live"));
    CHECK(contains(log, "01 - Opening.flac  CRC32 "));
    CHECK(contains(log, "Track  2  LBA     300  00:02.00  audio  Guest / \xE5\xA4\x9C\xE6\x98\x8E\xE3\x81\x91"));
    CHECK(contains(log, "\nAll tracks ripped without errors\n"));
}

TEST(accuraterip_no_match_hints_at_offset) {
    Rig rig;
    FakeHttp http;
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(rig.session.toc());
    http.accurateRip = reply(200, dbar(id, {{{9, 1}, {9, 2}, {9, 3}}}));
    rig.session.beginRip(settings("wav", 30));
    TempDir dir;
    rig.session.ripTrack(1, dir.path / "t.wav");
    const cdr::AccurateRipReport& report = rig.session.checkAccurateRip(&http);
    CHECK_EQ(report.accurateTracks(), 0);
    CHECK_EQ(report.tracksInDatabase(), 1);
    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "Not accurate (v2 0, v1 0 of 9 submissions; 0 of 1 pressings)"));
    CHECK(contains(log, "AccurateRip: 0 of 1 track(s) accurately ripped, 1 track(s) in database"));
    CHECK(contains(log, "Hint: no track matched. Check the read offset."));
}

TEST(failed_lookups_do_not_stop_the_rip) {
    Rig rig;
    FakeHttp http;  // every request fails at the transport level
    const cdr::CddbLookupResult& r = rig.session.lookupCddb(&http, {});
    CHECK(!r.found);
    CHECK_STR(r.error, "no route");
    CHECK(rig.session.albumDirectoryName().rfind("cd_", 0) == 0);

    rig.session.beginRip(settings("flac"));
    TempDir dir;
    const cdr::RippedTrack& t = rig.session.ripTrack(1, dir.path / "t.flac");
    CHECK_STR(t.fileName, "Track01.flac");
    CHECK(decodeFlac(readFile(dir.path / "t.flac")).pcm == referenceRip(1).pcm);

    const cdr::AccurateRipReport& report = rig.session.checkAccurateRip(&http);
    CHECK(report.status == cdr::AccurateRipReport::Status::Error);
    CHECK_STR(report.error, "no route");
    CHECK_EQ(report.tracks.size(), size_t(1));
    CHECK(!report.tracks.empty() && !report.tracks[0].inDatabase());
    std::string log = rig.session.ripLog();
    CHECK(contains(log, "CDDB lookup (" + std::string(cdr::kDefaultCddbServer) + "): no route"));
    CHECK(contains(log, "AccurateRip: lookup failed: no route"));
    CHECK(contains(log, "All tracks ripped without errors"));

    http.accurateRip = reply(404, "<html>Not Found</html>");
    CHECK(rig.session.checkAccurateRip(&http).status == cdr::AccurateRipReport::Status::NotFound);
    CHECK(contains(rig.session.ripLog(), "AccurateRip: this disc is not in the database"));

    http.accurateRip = reply(200, "garbage");
    CHECK(rig.session.checkAccurateRip(&http).status == cdr::AccurateRipReport::Status::Error);

    // An HTTP client that throws is contained as well.
    struct Throwing : cdr::HttpClient {
        cdr::HttpResponse get(const std::string&) override { throw std::runtime_error("boom"); }
    } throwing;
    CHECK(!rig.session.lookupCddb(&throwing, {}).found);
    CHECK(rig.session.checkAccurateRip(&throwing).status == cdr::AccurateRipReport::Status::Error);
    CHECK_EQ(rig.session.rippedTracks().size(), size_t(1));
}

TEST(disabled_lookups_send_no_requests) {
    Rig rig;
    FakeHttp http;
    addCddbAlbum(http);
    cdr::CddbSettings off;
    off.enabled = false;
    CHECK(!rig.session.lookupCddb(&http, off).found);
    rig.session.beginRip(settings("wav"));
    TempDir dir;
    rig.session.ripTrack(3, dir.path / "t.wav");
    const cdr::AccurateRipReport& report = rig.session.checkAccurateRip(nullptr);
    CHECK(report.status == cdr::AccurateRipReport::Status::Disabled);
    CHECK_EQ(report.tracks.size(), size_t(1));
    CHECK(http.urls.empty());
    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "CDDB lookup: disabled"));
    CHECK(contains(log, "AccurateRip: lookup disabled"));
    CHECK(contains(log, "Track03.wav  CRC32 "));
}

TEST(mcn_and_isrc_in_tags_and_log) {
    Rig rig;
    rig.fake.mcn = "4988001234567";
    rig.fake.isrcs[1] = "JPVI09912345";
    rig.fake.isrcs[3] = "JPVI09912347";
    FakeHttp http;
    addCddbAlbum(http);
    rig.session.lookupCddb(&http, {});
    CHECK_EQ(rig.fake.subChannelCommands, 0);  // read when the rip starts, not with the TOC
    rig.session.beginRip(settings("flac"));
    CHECK_EQ(rig.fake.subChannelCommands, 4);  // MCN + 3 ISRCs, through the BOT
    CHECK_STR(rig.session.album().mcn, "4988001234567");
    CHECK_STR(rig.session.trackMetadata(1).isrc, "JPVI09912345");
    CHECK_STR(rig.session.trackMetadata(2).isrc, "");
    // A second rip of the same disc does not read them again; a new CDDB
    // lookup keeps them.
    rig.session.beginRip(settings("flac"));
    rig.session.lookupCddb(&http, {});
    CHECK_EQ(rig.fake.subChannelCommands, 4);
    CHECK_STR(rig.session.trackMetadata(3).isrc, "JPVI09912347");
    CHECK_STR(rig.session.album().title, "Album: Live");

    TempDir dir;
    rig.session.ripTrack(1, dir.path / "1.flac");
    const DecodedFlac d = decodeFlac(readFile(dir.path / "1.flac"));
    CHECK(hasComment(d, "ISRC=JPVI09912345"));
    CHECK(hasComment(d, "BARCODE=4988001234567"));
    CHECK(hasComment(d, "TITLE=Opening"));
    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "MCN: 4988001234567\n"));
    CHECK(contains(log, "Track  1  ISRC: JPVI09912345\n"));
    CHECK(contains(log, "Track  2  ISRC: not present\n"));

    // Reading the TOC again (another disc) forgets them.
    rig.session.readToc();
    CHECK_STR(rig.session.album().mcn, "");
    CHECK(contains(rig.session.ripLog(), "MCN / ISRC: not read (disabled)"));
}

TEST(mcn_and_isrc_disabled_or_unsupported) {
    Rig rig;
    rig.fake.mcn = "4988001234567";
    cdr::RipSettings off = settings("wav");
    off.readDiscCodes = false;
    rig.session.beginRip(off);
    CHECK_EQ(rig.fake.subChannelCommands, 0);
    CHECK_STR(rig.session.album().mcn, "");

    // A drive that rejects READ SUB-CHANNEL: the rip goes on without codes.
    Rig old;
    old.fake.subChannelSupported = false;
    old.session.beginRip(settings("wav"));
    CHECK_EQ(old.fake.subChannelCommands, 1);
    TempDir dir;
    CHECK(old.session.ripTrack(2, dir.path / "2.wav").result.clean());
    const std::string log = old.session.ripLog();
    CHECK(contains(log, "MCN: not supported by the drive\n"));
    CHECK(contains(log, "Track  2  ISRC: not read (command not supported)\n"));
}

TEST(cancel_stops_the_rip) {
    Rig rig;
    rig.session.beginRip(settings("flac"));
    TempDir dir;
    int calls = 0;
    bool cancelled = false;
    try {
        rig.session.ripTrack(1, dir.path / "t.flac", [&](uint32_t, uint32_t) {
            if (++calls == 2) rig.session.cancel();
        });
    } catch (const cdr::RipCancelled&) {
        cancelled = true;
    }
    CHECK(cancelled);
    CHECK_EQ(calls, 2);
    CHECK(rig.session.rippedTracks().empty());

    // Further tracks of the cancelled rip stop right away...
    cancelled = false;
    const int commands = rig.device.commands;
    try {
        rig.session.ripTrack(2, dir.path / "t.flac");
    } catch (const cdr::RipCancelled&) {
        cancelled = true;
    }
    CHECK(cancelled);
    CHECK_EQ(rig.device.commands, commands);

    // ...and the next rip works again on the same session.
    rig.session.beginRip(settings("flac"));
    const cdr::RippedTrack& t = rig.session.ripTrack(1, dir.path / "t.flac");
    CHECK(t.result.clean());
    CHECK(decodeFlac(readFile(dir.path / "t.flac")).pcm == referenceRip(1).pcm);
}

TEST(unreadable_sectors_are_reported) {
    Rig rig;
    rig.fake.failuresBySector[320] = -1;
    rig.session.beginRip(settings("wav"));
    TempDir dir;
    const cdr::RippedTrack& t = rig.session.ripTrack(2, dir.path / "t.wav");
    CHECK_EQ(t.result.unreadableSectors, 1u);
    CHECK_EQ(rig.session.problemTracks(), 1);
    const std::string log = rig.session.ripLog();
    CHECK(contains(log, "1 unreadable sector(s)"));
    CHECK(contains(log, "\nFinished with errors\n"));
}

int main() {
    for (auto& [name, fn] : registry()) {
        const int before = failures;
        try {
            fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  unexpected exception: %s\n", e.what());
            ++failures;
        }
        std::printf("%s %s\n", failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%s (%zu tests)\n", failures ? "FAILED" : "PASSED", registry().size());
    return failures ? 1 : 0;
}
