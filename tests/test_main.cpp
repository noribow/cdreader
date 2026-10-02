// Minimal self-contained test runner (no external dependencies).

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/crc32.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/file_naming.h"
#include "cdreader/flac_encoder.h"
#include "cdreader/flac_writer.h"
#include "cdreader/http.h"
#include "cdreader/md5.h"
#include "cdreader/metadata.h"
#include "cdreader/ogg.h"
#include "cdreader/ripper.h"
#include "cdreader/scsi.h"
#include "cdreader/tags.h"
#include "cdreader/toc.h"
#include "cdreader/wav_writer.h"
#include "fake_drive.h"
#include "flac_decoder.h"
#include "test_signals.h"

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

// Three audio tracks; lead-out at 10 seconds.
FakeDrive makeAudioDisc() { return FakeDrive({{0, false}, {300, false}, {450, false}}, 750); }

std::vector<uint8_t> expectedTrackData(uint32_t start, uint32_t length) {
    std::vector<uint8_t> v(size_t(length) * cdr::kSectorBytes);
    for (uint32_t s = 0; s < length; ++s)
        for (size_t b = 0; b < cdr::kSectorBytes; ++b)
            v[size_t(s) * cdr::kSectorBytes + b] = FakeDrive::sampleByte(start + s, b);
    return v;
}

// Offset-corrected reference: the bytes starting `offsetSamples` after the
// track start, with silence wherever that falls outside [readableBegin, readableEnd).
std::vector<uint8_t> expectedWithOffset(uint32_t start, uint32_t length, int offsetSamples,
                                        uint32_t readableBegin, uint32_t readableEnd) {
    std::vector<uint8_t> v(size_t(length) * cdr::kSectorBytes);
    const int64_t firstByte = (int64_t(start) * cdr::kSamplesPerSector + offsetSamples) * cdr::kBytesPerSample;
    for (size_t i = 0; i < v.size(); ++i) {
        const int64_t abs = firstByte + int64_t(i);
        const int64_t lba = abs >= 0 ? abs / cdr::kSectorBytes : -1;
        if (lba < int64_t(readableBegin) || lba >= int64_t(readableEnd)) continue;
        v[i] = FakeDrive::sampleByte(uint32_t(lba), size_t(abs % cdr::kSectorBytes));
    }
    return v;
}

struct OffsetRip {
    cdr::TrackRipResult result;
    std::vector<uint8_t> bytes;
};

OffsetRip ripWithOffset(FakeDrive& fake, int trackNumber, int offset) {
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::RipOptions options;
    options.readOffsetSamples = offset;
    cdr::Ripper ripper(drive, toc, options);
    OffsetRip out;
    out.result = ripper.ripTrack(*toc.findTrack(trackNumber), [&](const uint8_t* p, size_t n) {
        out.bytes.insert(out.bytes.end(), p, p + n);
    });
    return out;
}

struct Collected {
    std::vector<uint8_t> bytes;
    cdr::Ripper::SampleSink sink() {
        return [this](const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p + n); };
    }
};

// Answers CDDB requests from canned bodies keyed by the command ("cddb query",
// "cddb read"); records every URL requested.
struct FakeHttp : cdr::HttpClient {
    std::map<std::string, cdr::HttpResponse> byCommand;
    std::vector<std::string> urls;

    void reply(const std::string& command, const std::string& body, int status = 200) {
        cdr::HttpResponse r;
        r.ok = true;
        r.status = status;
        r.body = body;
        byCommand[command] = r;
    }

    cdr::HttpResponse get(const std::string& url) override {
        urls.push_back(url);
        const size_t cmd = url.find("cmd=");
        const size_t end = url.find('&', cmd);
        const std::string encoded = url.substr(cmd + 4, end - cmd - 4);
        for (const auto& [prefix, response] : byCommand)
            if (encoded.rfind(cdr::urlEncode(prefix), 0) == 0) return response;
        cdr::HttpResponse r;
        r.error = "no route";
        return r;
    }
};

// 3 tracks at 0 s / 200 s / 400 s, lead-out at 600 s (LBA).
cdr::Toc makeCddbToc() {
    cdr::Toc toc;
    toc.firstTrack = 1;
    toc.lastTrack = 3;
    toc.tracks = {{1, 0}, {2, 15000}, {3, 30000}};
    toc.leadOutLba = 45000;
    return toc;
}

}  // namespace

TEST(inquiry_reads_vendor_and_product) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::DriveInfo info = drive.inquiry();
    CHECK(info.vendor == "FAKE");
    CHECK(info.product == "CD-ROM DRIVE");
    CHECK(info.revision == "1.00");
    CHECK(info.displayName() == "FAKE CD-ROM DRIVE (1.00)");
}

TEST(toc_parses_track_lengths) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    CHECK_EQ(toc.firstTrack, 1);
    CHECK_EQ(toc.lastTrack, 3);
    CHECK_EQ(toc.tracks.size(), 3u);
    CHECK_EQ(toc.leadOutLba, 750u);
    CHECK_EQ(toc.tracks[0].lengthSectors, 300u);
    CHECK_EQ(toc.tracks[1].lengthSectors, 150u);
    CHECK_EQ(toc.tracks[2].lengthSectors, 300u);
    CHECK_EQ(toc.audioTrackCount(), 3u);
    CHECK(toc.findTrack(2) != nullptr && toc.findTrack(2)->startLba == 300);
    CHECK(toc.findTrack(4) == nullptr);
}

TEST(toc_enhanced_cd_excludes_session_gap) {
    // Audio tracks followed by a data track in a second session.
    FakeDrive fake({{0, false}, {20000, false}, {50000, true}}, 60000);
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    CHECK(toc.tracks[1].isAudio);
    CHECK(!toc.tracks[2].isAudio);
    CHECK_EQ(toc.tracks[1].lengthSectors, 50000u - 20000u - cdr::kSessionGapSectors);
    CHECK_EQ(toc.audioTrackCount(), 2u);
}

TEST(toc_rejects_garbage) {
    uint8_t tooShort[2] = {0, 0};
    bool threw = false;
    try {
        cdr::Toc::parse(tooShort, sizeof tooShort);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(cddb_id_matches_reference) {
    // Worked example of the freedb algorithm.
    cdr::Toc toc;
    toc.tracks = {{1, 0}, {2, 15000}, {3, 30000}};
    toc.leadOutLba = 45000;
    // seconds: 2, 202, 402 -> digit sums 2, 4, 6 = 12; length 600 s
    CHECK_EQ(toc.cddbId(), (12u << 24) | (600u << 8) | 3u);
}

TEST(msf_formatting) {
    CHECK(cdr::formatMsf(0) == "00:00.00");
    CHECK(cdr::formatMsf(75 * 61 + 3) == "01:01.03");
}

TEST(sense_parsing_fixed_and_descriptor) {
    uint8_t fixed[18] = {0x70, 0, 0x03, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x11, 0x05};
    cdr::SenseInfo s = cdr::parseSense(fixed, sizeof fixed);
    CHECK_EQ(s.key, 0x3);
    CHECK_EQ(s.asc, 0x11);
    CHECK_EQ(s.ascq, 0x05);
    uint8_t desc[8] = {0x72, 0x02, 0x3A, 0x01};
    s = cdr::parseSense(desc, sizeof desc);
    CHECK_EQ(s.key, 0x2);
    CHECK_EQ(s.asc, 0x3A);
    CHECK_EQ(s.ascq, 0x01);
}

TEST(crc32_known_vector) {
    cdr::Crc32 crc;
    const char* text = "123456789";
    crc.update(reinterpret_cast<const uint8_t*>(text), 4);
    crc.update(reinterpret_cast<const uint8_t*>(text) + 4, 5);
    CHECK_EQ(crc.value(), 0xCBF43926u);
}

TEST(rip_clean_track_returns_exact_audio) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, {});
    Collected out;
    uint32_t lastDone = 0, lastTotal = 0;
    cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(2), out.sink(), [&](uint32_t d, uint32_t t) {
        lastDone = d;
        lastTotal = t;
    });
    CHECK(r.clean());
    CHECK_EQ(r.retries, 0u);
    CHECK_EQ(r.sectors, 150u);
    CHECK_EQ(lastDone, 150u);
    CHECK_EQ(lastTotal, 150u);
    const std::vector<uint8_t> expected = expectedTrackData(300, 150);
    CHECK(out.bytes == expected);
    cdr::Crc32 crc;
    crc.update(expected.data(), expected.size());
    CHECK_EQ(r.crc32, crc.value());
}

TEST(rip_retries_transient_errors) {
    FakeDrive fake = makeAudioDisc();
    fake.failuresBySector[310] = 2;
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, {});
    Collected out;
    cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(2), out.sink());
    CHECK(r.clean());
    CHECK_EQ(r.retries, 2u);
    CHECK(out.bytes == expectedTrackData(300, 150));
}

TEST(rip_isolates_unreadable_sector) {
    FakeDrive fake = makeAudioDisc();
    fake.failuresBySector[320] = -1;
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, {2, false});
    Collected out;
    cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(2), out.sink());
    CHECK_EQ(r.unreadableSectors, 1u);
    CHECK(!r.clean());
    std::vector<uint8_t> expected = expectedTrackData(300, 150);
    std::memset(expected.data() + size_t(20) * cdr::kSectorBytes, 0, cdr::kSectorBytes);
    CHECK_EQ(out.bytes.size(), expected.size());
    CHECK(out.bytes == expected);  // neighbours of the bad sector are intact
}

TEST(rip_verify_detects_unstable_data) {
    FakeDrive fake = makeAudioDisc();
    fake.unstableSectors[5] = true;
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, {1, true});
    Collected out;
    cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(1), out.sink());
    CHECK_EQ(r.unreadableSectors, 1u);
    CHECK(r.retries > 0);
}

TEST(rip_verify_clean_disc_reads_twice) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, {0, true});
    Collected out;
    cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(1), out.sink());
    CHECK(r.clean());
    const int blocks = int((300 + cdr::kMaxSectorsPerRead - 1) / cdr::kMaxSectorsPerRead);
    CHECK_EQ(fake.readCommands, 2 * blocks);
}

TEST(audio_range_spans_consecutive_audio_tracks) {
    FakeDrive fake({{0, true}, {5000, false}, {8000, false}, {20000, true}}, 30000);
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::Toc::LbaRange r = toc.audioRange(*toc.findTrack(3));
    CHECK_EQ(r.begin, 5000u);
    CHECK_EQ(r.end, 20000u - cdr::kSessionGapSectors);
}

TEST(offset_zero_matches_plain_rip) {
    FakeDrive fake = makeAudioDisc();
    OffsetRip r = ripWithOffset(fake, 2, 0);
    CHECK(r.bytes == expectedTrackData(300, 150));
    CHECK_EQ(r.result.paddedSamples, 0u);
}

TEST(offset_positive_shifts_into_next_track) {
    for (int offset : {6, 48, 588, 667, 1176 + 30}) {
        FakeDrive fake = makeAudioDisc();
        OffsetRip r = ripWithOffset(fake, 2, offset);
        CHECK_EQ(r.bytes.size(), size_t(150) * cdr::kSectorBytes);
        CHECK(r.bytes == expectedWithOffset(300, 150, offset, 0, 750));
        CHECK_EQ(r.result.paddedSamples, 0u);
        CHECK(r.result.clean());
    }
}

TEST(offset_negative_shifts_into_previous_track) {
    for (int offset : {-6, -472, -588, -1500}) {
        FakeDrive fake = makeAudioDisc();
        OffsetRip r = ripWithOffset(fake, 2, offset);
        CHECK(r.bytes == expectedWithOffset(300, 150, offset, 0, 750));
        CHECK_EQ(r.result.paddedSamples, 0u);
    }
}

TEST(offset_pads_silence_past_lead_out) {
    FakeDrive fake = makeAudioDisc();
    OffsetRip r = ripWithOffset(fake, 3, 667);
    CHECK(r.bytes == expectedWithOffset(450, 300, 667, 0, 750));
    CHECK_EQ(r.result.paddedSamples, 667u);
    CHECK(r.result.clean());
    const size_t tail = 667 * cdr::kBytesPerSample;  // the last 667 samples are silence
    CHECK(std::all_of(r.bytes.end() - tail, r.bytes.end(), [](uint8_t b) { return b == 0; }));
}

TEST(offset_pads_silence_before_first_sector) {
    FakeDrive fake = makeAudioDisc();
    OffsetRip r = ripWithOffset(fake, 1, -30);
    CHECK(r.bytes == expectedWithOffset(0, 300, -30, 0, 750));
    CHECK_EQ(r.result.paddedSamples, 30u);
    CHECK(r.result.clean());
}

TEST(offset_does_not_read_into_data_session) {
    // The sectors between the audio session and the data track must not be read.
    FakeDrive fake({{0, false}, {20000, false}, {50000, true}}, 60000);
    const uint32_t audioEnd = 50000 - cdr::kSessionGapSectors;
    for (uint32_t s = audioEnd; s < audioEnd + 4; ++s) fake.failuresBySector[s] = -1;
    OffsetRip r = ripWithOffset(fake, 2, 700);
    CHECK(r.result.clean());
    CHECK_EQ(r.result.paddedSamples, 700u);
    CHECK(r.bytes == expectedWithOffset(20000, audioEnd - 20000, 700, 0, audioEnd));
}

TEST(album_metadata_for_track) {
    cdr::AlbumMetadata album;
    album.artist = "Artist";
    album.title = "Album";
    album.year = "1999";
    album.discId = "0A0B0C03";
    album.trackTitles = {"One", "Two"};
    album.trackArtists = {"", "Guest"};
    cdr::TrackMetadata m1 = album.forTrack(1, 3);
    CHECK(m1.title == "One" && m1.artist == "Artist" && m1.album == "Album" && m1.albumArtist == "Artist");
    CHECK_EQ(m1.trackNumber, 1);
    CHECK_EQ(m1.trackTotal, 3);
    CHECK(album.forTrack(2, 3).artist == "Guest");
    cdr::TrackMetadata m3 = album.forTrack(3, 3);  // no title known
    CHECK(m3.title.empty() && m3.year == "1999" && m3.discId == "0A0B0C03");
}

TEST(audio_writer_factory) {
    const std::vector<std::string> formats = cdr::audioFormats();
    CHECK(std::find(formats.begin(), formats.end(), "wav") != formats.end());
    for (const std::string& f : formats) {
        std::unique_ptr<cdr::AudioWriter> w = cdr::createAudioWriter(f);
        CHECK(w != nullptr && w->extension() == f);
    }
    CHECK(cdr::createAudioWriter("no-such-format") == nullptr);
}

TEST(not_ready_without_disc) {
    FakeDrive fake = makeAudioDisc();
    fake.discPresent = false;
    cdr::CdDrive drive(fake);
    CHECK(!drive.isReady());
    bool threw = false;
    try {
        drive.readToc();
    } catch (const cdr::ScsiError& e) {
        threw = true;
        CHECK_EQ(e.result().sense.asc, 0x3A);
    }
    CHECK(threw);
}

TEST(wav_writer_produces_valid_header) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_test.wav";
    const std::vector<uint8_t> pcm = expectedTrackData(0, 2);
    {
        cdr::WavWriter wav;
        wav.open(path);
        wav.write(pcm.data(), 1000);
        wav.write(pcm.data() + 1000, pcm.size() - 1000);
        wav.close();
        CHECK_EQ(wav.dataBytes(), uint64_t(pcm.size()));
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);

    auto le32 = [&](size_t o) {
        return uint32_t(file[o]) | uint32_t(file[o + 1]) << 8 | uint32_t(file[o + 2]) << 16 | uint32_t(file[o + 3]) << 24;
    };
    auto le16 = [&](size_t o) { return uint32_t(file[o]) | uint32_t(file[o + 1]) << 8; };
    CHECK_EQ(file.size(), 44u + pcm.size());
    CHECK(std::memcmp(file.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(file.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK_EQ(le32(4), uint32_t(36 + pcm.size()));
    CHECK_EQ(le16(20), 1u);      // PCM
    CHECK_EQ(le16(22), 2u);      // stereo
    CHECK_EQ(le32(24), 44100u);  // sample rate
    CHECK_EQ(le32(28), 176400u); // byte rate
    CHECK_EQ(le16(32), 4u);      // block align
    CHECK_EQ(le16(34), 16u);     // bits per sample
    CHECK(std::memcmp(file.data() + 36, "data", 4) == 0);
    CHECK_EQ(le32(40), uint32_t(pcm.size()));
    CHECK(std::equal(pcm.begin(), pcm.end(), file.begin() + 44));
}

TEST(cddb_query_command_follows_freedb_spec) {
    const cdr::Toc toc = makeCddbToc();
    // Disc id: digit sums of 2, 202, 402 s = 12; length 602 - 2 = 600 s = 0x258; 3 tracks.
    // Offsets include the 150 frame pregap; nsecs is the lead-out (45150 / 75 = 602 s).
    CHECK(cdr::cddbQueryCommand(toc) == "cddb query 0c025803 3 150 15150 30150 602");

    // CD-Extra: the data track is part of the query like any other track.
    cdr::Toc extra;
    extra.tracks = {{1, 0}, {2, 20000}, {3, 50000, 0, false}};
    extra.leadOutLba = 60000;
    // seconds 2, 268, 668 -> 2 + 16 + 20 = 38 = 0x26; length 802 - 2 = 800 = 0x320
    CHECK(cdr::cddbQueryCommand(extra) == "cddb query 26032003 3 150 20150 50150 802");
}

TEST(cddb_request_url_is_encoded) {
    CHECK(cdr::urlEncode("a b&c=d/\xC3\xA9~") == "a+b%26c%3Dd%2F%C3%A9~");
    cdr::CddbClientInfo client;
    client.user = "my user";
    client.version = "1.2.3";
    const std::string url = cdr::cddbRequestUrl("https://example.org/~cddb/cddb.cgi", "cddb read rock 0c025803", client);
    CHECK(url ==
          "https://example.org/~cddb/cddb.cgi?cmd=cddb+read+rock+0c025803&hello=my_user+localhost+cdreader+1.2.3"
          "&proto=6");
    CHECK(cdr::cddbRequestUrl("http://x/cgi?a=1", "cddb query", client).rfind("http://x/cgi?a=1&cmd=cddb+query&", 0) ==
          0);
}

TEST(cddb_query_response_codes) {
    using S = cdr::CddbQueryResult::Status;
    cdr::CddbQueryResult r = cdr::parseCddbQueryResponse("200 rock 0c025803 Some Artist / Some Album\r\n");
    CHECK(r.status == S::Exact);
    CHECK_EQ(r.code, 200);
    CHECK_EQ(r.matches.size(), 1u);
    CHECK(r.matches[0].category == "rock" && r.matches[0].discId == "0c025803" &&
          r.matches[0].title == "Some Artist / Some Album");

    r = cdr::parseCddbQueryResponse(
        "210 Found exact matches, list follows (until terminating `.')\r\n"
        "rock 0c025803 A / B\r\nmisc 0c025803 C / D\r\n.\r\n");
    CHECK(r.status == S::Exact);
    CHECK_EQ(r.matches.size(), 2u);
    CHECK(r.matches[1].category == "misc" && r.matches[1].title == "C / D");

    r = cdr::parseCddbQueryResponse(
        "211 Found inexact matches, list follows (until terminating `.')\n"
        "jazz 0c025804 E / F\n.\n");
    CHECK(r.status == S::Inexact);
    CHECK_EQ(r.code, 211);
    CHECK_EQ(r.matches.size(), 1u);

    r = cdr::parseCddbQueryResponse("202 No match found\r\n");
    CHECK(r.status == S::NotFound);
    CHECK(r.matches.empty());

    r = cdr::parseCddbQueryResponse("403 Database entry is corrupt.\r\n");
    CHECK(r.status == S::Error);
    CHECK_EQ(r.code, 403);
    CHECK(r.message == "403 Database entry is corrupt.");

    CHECK(cdr::parseCddbQueryResponse("").status == S::Error);
    CHECK(cdr::parseCddbQueryResponse("<html><body>Bad Gateway</body></html>").status == S::Error);
    CHECK(cdr::parseCddbQueryResponse("211 list follows\n.\n").status == S::Error);
}

TEST(cddb_xmcd_entry_parsing) {
    const std::string entry =
        "# xmcd\n"
        "#\n"
        "# Track frame offsets:\n"
        "#        150\n"
        "# DTITLE=commented out\n"
        "DISCID=0c025803\n"
        "DTITLE=The Artist / The Album\n"
        "DYEAR=1999\n"
        "DGENRE=Rock\n"
        "TTITLE0=First\n"
        "TTITLE1=A very long title that the server spl\n"
        "TTITLE1=it over two lines\n"
        "TTITLE2=Tab\\there, new\\nline, back\\\\slash, other \\x\n"
        "EXTD=\n"
        "PLAYORDER=\n";
    const cdr::AlbumMetadata a = cdr::parseXmcd(entry);
    CHECK(a.artist == "The Artist");
    CHECK(a.title == "The Album");
    CHECK(a.year == "1999");
    CHECK(a.genre == "Rock");
    CHECK_EQ(a.trackTitles.size(), 3u);
    CHECK(a.trackTitles[0] == "First");
    CHECK(a.trackTitles[1] == "A very long title that the server split over two lines");
    CHECK(a.trackTitles[2] == "Tab here, new line, back\\slash, other \\x");
    CHECK(a.trackArtists.empty());

    // No " / ": artist and title are the same (xmcd spec). DTITLE may be split too.
    const cdr::AlbumMetadata b = cdr::parseXmcd("DTITLE=Self Tit\nDTITLE=led\nTTITLE1=Second only\n");
    CHECK(b.artist == "Self Titled" && b.title == "Self Titled");
    CHECK_EQ(b.trackTitles.size(), 2u);
    CHECK(b.trackTitles[0].empty() && b.trackTitles[1] == "Second only");
}

TEST(cddb_compilation_track_artists) {
    const cdr::AlbumMetadata various = cdr::parseXmcd(
        "DTITLE=Various Artists / Hits\n"
        "TTITLE0=Singer A / Song A\n"
        "TTITLE1=Song without artist\n"
        "TTITLE2=Singer C / Song C / Remix\n");
    CHECK(various.artist == "Various Artists");
    CHECK_EQ(various.trackArtists.size(), 3u);
    CHECK(various.trackArtists[0] == "Singer A" && various.trackTitles[0] == "Song A");
    CHECK(various.trackArtists[1].empty() && various.trackTitles[1] == "Song without artist");
    CHECK(various.trackArtists[2] == "Singer C" && various.trackTitles[2] == "Song C / Remix");
    CHECK(various.forTrack(1, 3).artist == "Singer A");
    CHECK(various.forTrack(2, 3).artist == "Various Artists");

    // Not marked as various, but every track has an artist.
    const cdr::AlbumMetadata split = cdr::parseXmcd("DTITLE=DJ / Mix\nTTITLE0=X / One\nTTITLE1=Y / Two\n");
    CHECK(split.trackArtists.size() == 2 && split.trackArtists[1] == "Y" && split.trackTitles[1] == "Two");

    // A single title containing " / " on a normal album is left alone.
    const cdr::AlbumMetadata normal = cdr::parseXmcd("DTITLE=Band / LP\nTTITLE0=Intro\nTTITLE1=This / That\n");
    CHECK(normal.trackArtists.empty());
    CHECK(normal.trackTitles[1] == "This / That");
}

TEST(cddb_text_encoding) {
    // Valid UTF-8 (Japanese) is kept.
    const std::string jp = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";  // テスト
    CHECK(cdr::toValidUtf8(jp) == jp);
    const cdr::AlbumMetadata a = cdr::parseXmcd("DTITLE=" + jp + " / " + jp + "\nTTITLE0=" + jp + "\n");
    CHECK(a.artist == jp && a.trackTitles[0] == jp);

    // Latin-1 from old entries: "Café" with E9.
    CHECK(cdr::toValidUtf8("Caf\xE9") == "Caf\xC3\xA9");
    const cdr::AlbumMetadata b = cdr::parseXmcd("DTITLE=Bj\xF6rk / Debut\n");
    CHECK(b.artist == "Bj\xC3\xB6rk");
    // Truncated sequence, overlong form and surrogate are all invalid.
    CHECK(cdr::toValidUtf8("\xE3\x83") == "\xC3\xA3\xC2\x83");
    CHECK(cdr::toValidUtf8("\xC0\xAF") == "\xC3\x80\xC2\xAF");
    CHECK(cdr::toValidUtf8("\xED\xA0\x80") != "\xED\xA0\x80");
    CHECK(cdr::toValidUtf8("\xF0\x9F\x8E\xB5") == "\xF0\x9F\x8E\xB5");  // 4 byte sequence is fine
}

TEST(cddb_read_response) {
    cdr::AlbumMetadata album;
    std::string error;
    CHECK(cdr::parseCddbReadResponse("210 rock 0c025803 CD database entry follows (until terminating `.')\r\n"
                                     "DTITLE=A / B\r\nTTITLE0=T\r\n.\r\nTTITLE0=after the end\r\n",
                                     album, error));
    CHECK(album.artist == "A" && album.title == "B");
    CHECK(album.trackTitles.size() == 1 && album.trackTitles[0] == "T");

    CHECK(!cdr::parseCddbReadResponse("401 rock 0c025803 No such CD entry in database.\r\n", album, error));
    CHECK(error == "401 rock 0c025803 No such CD entry in database.");
    CHECK(!cdr::parseCddbReadResponse("", album, error));
}

TEST(cddb_lookup_end_to_end) {
    FakeHttp http;
    http.reply("cddb query",
               "211 Found inexact matches, list follows (until terminating `.')\r\n"
               "rock 0c025803 First / Match\r\nmisc 0c025803 Second / Match\r\n.\r\n");
    http.reply("cddb read rock", "210 rock 0c025803\r\nDTITLE=First / Match\r\nTTITLE0=One\r\n.\r\n");
    http.reply("cddb read misc",
               "210 misc 0c025803\r\nDTITLE=Second / Match\r\nDYEAR=2001\r\nTTITLE0=Uno\r\nTTITLE1=Dos\r\n.\r\n");

    cdr::CddbOptions options;
    options.server = "https://cddb.example/cgi";
    cdr::CddbLookupResult r = cdr::lookupCddb(http, makeCddbToc(), options);
    CHECK(r.found);
    CHECK(!r.exact);
    CHECK_EQ(r.matches.size(), 2u);
    CHECK_EQ(r.chosen, 0u);
    CHECK(r.album.artist == "First" && r.album.trackTitles[0] == "One");
    CHECK(r.album.discId == "0C025803");
    CHECK_EQ(http.urls.size(), 2u);
    CHECK(http.urls[0] ==
          "https://cddb.example/cgi?cmd=cddb+query+0c025803+3+150+15150+30150+602"
          "&hello=cdreader+localhost+cdreader+0.1.0&proto=6");

    options.matchIndex = 1;
    r = cdr::lookupCddb(http, makeCddbToc(), options);
    CHECK(r.found && r.chosen == 1 && r.album.title == "Match" && r.album.year == "2001");
    CHECK(r.album.forTrack(2, 3).title == "Dos");

    options.matchIndex = 5;  // out of range: falls back to the first match
    r = cdr::lookupCddb(http, makeCddbToc(), options);
    CHECK(r.found && r.chosen == 0);
}

TEST(cddb_lookup_failures_do_not_throw) {
    const cdr::Toc toc = makeCddbToc();
    FakeHttp none;  // every request fails at the transport level
    cdr::CddbLookupResult r = cdr::lookupCddb(none, toc);
    CHECK(!r.found && r.error == "no route");

    FakeHttp notFound;
    notFound.reply("cddb query", "202 No match found\r\n");
    r = cdr::lookupCddb(notFound, toc);
    CHECK(!r.found && !r.error.empty() && notFound.urls.size() == 1);

    FakeHttp httpError;
    httpError.reply("cddb query", "oops", 503);
    r = cdr::lookupCddb(httpError, toc);
    CHECK(!r.found && r.error == "HTTP status 503");

    FakeHttp readFails;
    readFails.reply("cddb query", "200 rock 0c025803 A / B\r\n");
    readFails.reply("cddb read", "402 Server error.\r\n");
    r = cdr::lookupCddb(readFails, toc);
    CHECK(!r.found && r.error == "read failed: 402 Server error." && r.matches.size() == 1);

    struct Throwing : cdr::HttpClient {
        cdr::HttpResponse get(const std::string&) override { throw std::runtime_error("boom"); }
    } throwing;
    r = cdr::lookupCddb(throwing, toc);
    CHECK(!r.found && r.error == "boom");
}

TEST(cddb_lookup_disc_starting_after_track_one) {
    cdr::Toc toc = makeCddbToc();
    for (size_t i = 0; i < toc.tracks.size(); ++i) toc.tracks[i].number = int(i) + 2;
    FakeHttp http;
    http.reply("cddb query", "200 rock 0c025803 A / B\r\n");
    http.reply("cddb read", "210 rock 0c025803\r\nDTITLE=A / B\r\nTTITLE0=Two\r\nTTITLE1=Three\r\n.\r\n");
    const cdr::CddbLookupResult r = cdr::lookupCddb(http, toc);
    CHECK(r.found);
    CHECK(r.album.forTrack(2, 4).title == "Two");
    CHECK(r.album.forTrack(3, 4).title == "Three");
}

TEST(file_name_sanitizing) {
    CHECK(cdr::sanitizeFileName("AC/DC: \"Live\" <1991>?*|\\") == "AC_DC_ _Live_ _1991_____");
    CHECK(cdr::sanitizeFileName("tab\there\x01") == "tab_here_");
    CHECK(cdr::sanitizeFileName("  leading and trailing. . ") == "leading and trailing");
    CHECK(cdr::sanitizeFileName("...").empty());
    CHECK(cdr::sanitizeFileName("con") == "_con");
    CHECK(cdr::sanitizeFileName("CON.txt") == "_CON.txt");
    CHECK(cdr::sanitizeFileName("Com3") == "_Com3");
    CHECK(cdr::sanitizeFileName("COM0") == "COM0");
    CHECK(cdr::sanitizeFileName("Console") == "Console");
    const std::string jp = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";  // 日本語 (3 x 3 bytes)
    CHECK(cdr::sanitizeFileName(jp + "?") == jp + "_");
    // Cut at a character boundary, never inside a multi-byte sequence.
    CHECK(cdr::sanitizeFileName(jp, 7) == "\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(cdr::sanitizeFileName(jp, 9) == jp);
}

TEST(file_names_from_metadata) {
    cdr::AlbumMetadata album;
    album.discId = "0C025803";
    album.trackTitles = {"Intro", "What?", ""};
    CHECK(cdr::trackFileBaseName(album.forTrack(1, 3)) == "01 - Intro");
    CHECK(cdr::trackFileBaseName(album.forTrack(2, 3)) == "02 - What_");
    CHECK(cdr::trackFileBaseName(album.forTrack(3, 3)) == "Track03");
    CHECK(cdr::trackFileBaseName(album.forTrack(12, 12)) == "Track12");
    CHECK(cdr::albumDirectoryName(album) == "cd_0C025803");
    album.title = "Album.";
    CHECK(cdr::albumDirectoryName(album) == "Album");
    album.artist = "Artist/Name";
    CHECK(cdr::albumDirectoryName(album) == "Artist_Name - Album");
    album.artist = album.title;
    CHECK(cdr::albumDirectoryName(album) == "Album");
}

// --- AccurateRip --------------------------------------------------------------
// Reference values were cross-checked with an independent reimplementation of
// ARver / whipper (which reproduces ARver's own published test vectors).

namespace {

// Builds a TOC from audio track lengths like ARver's DiscInfo.from_track_lengths:
// the first track starts after `pregap` sectors (hidden track), an optional
// data track follows in a second session (Enhanced CD).
cdr::Toc tocFromLengths(const std::vector<uint32_t>& lengths, uint32_t pregap, uint32_t dataLength) {
    cdr::Toc toc;
    uint32_t lba = pregap;
    int number = 1;
    for (uint32_t length : lengths) {
        cdr::Track t;
        t.number = number++;
        t.startLba = lba;
        t.lengthSectors = length;
        toc.tracks.push_back(t);
        lba += length;
    }
    if (dataLength) {
        cdr::Track t;
        t.number = number++;
        t.startLba = lba + cdr::kSessionGapSectors;
        t.lengthSectors = dataLength;
        t.isAudio = false;
        toc.tracks.push_back(t);
        lba = t.startLba + dataLength;
    }
    toc.firstTrack = 1;
    toc.lastTrack = number - 1;
    toc.leadOutLba = lba;
    return toc;
}

std::vector<uint8_t> samplesToBytes(const std::vector<uint32_t>& samples) {
    std::vector<uint8_t> bytes;
    for (uint32_t s : samples)
        for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(s >> (8 * i)));
    return bytes;
}

std::string bytesToString(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

// Real database responses (from ARver's test data).
const std::vector<uint8_t> kDbar001 = {
    0x01, 0xfd, 0x41, 0x04, 0x00, 0xfb, 0x83, 0x08, 0x00, 0x01, 0x88, 0x0e, 0x02, 0x07, 0x1e,
    0x54, 0x16, 0x27, 0x6b, 0xca, 0xd5, 0x07, 0x01, 0xfd, 0x41, 0x04, 0x00, 0xfb, 0x83, 0x08,
    0x00, 0x01, 0x88, 0x0e, 0x02, 0x06, 0x86, 0x54, 0x34, 0x80, 0x00, 0x00, 0x00, 0x00};
const std::vector<uint8_t> kDbar013 = {
    0x0d, 0x91, 0x67, 0x20, 0x00, 0x82, 0x6a, 0x48, 0x01, 0x0d, 0xde, 0x10, 0xa7, 0x02, 0xa2, 0x6e, 0xe2,
    0xe4, 0x44, 0xef, 0x9f, 0x36, 0x02, 0x6c, 0x4b, 0x21, 0xcf, 0xe8, 0x70, 0xa3, 0x3c, 0x02, 0x0e, 0x63,
    0x84, 0x21, 0x20, 0x57, 0xc4, 0x83, 0x02, 0xa9, 0x20, 0x2a, 0xd0, 0xf6, 0x4b, 0xde, 0x38, 0x02, 0x82,
    0x85, 0xef, 0x55, 0x89, 0x83, 0x36, 0x90, 0x02, 0xbd, 0x8a, 0x7c, 0xcf, 0xa6, 0x6c, 0x72, 0xad, 0x02,
    0xba, 0xc1, 0xd9, 0x48, 0x36, 0xbe, 0x5d, 0x6f, 0x02, 0xda, 0x75, 0x6c, 0x46, 0x10, 0x75, 0x6f, 0x1b,
    0x02, 0x23, 0x9e, 0x46, 0xf9, 0x6f, 0x9a, 0x2e, 0x91, 0x02, 0x71, 0x59, 0x84, 0x56, 0xdf, 0xd4, 0x0e,
    0x1b, 0x02, 0x9a, 0xa1, 0xd1, 0x51, 0x11, 0xb2, 0x02, 0x1d, 0x02, 0x73, 0xfa, 0xb3, 0x8a, 0x56, 0xb6,
    0x81, 0xf3, 0x02, 0xde, 0xe0, 0x49, 0x86, 0xe3, 0xca, 0x83, 0xd5};

// Returns the same response to every request (AccurateRip lookups).
class CannedHttp : public cdr::HttpClient {
public:
    cdr::HttpResponse response;
    std::vector<std::string> requests;

    cdr::HttpResponse get(const std::string& url) override {
        requests.push_back(url);
        return response;
    }
};

cdr::HttpResponse httpReply(int status, std::string body = {}) {
    cdr::HttpResponse r;
    r.ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
}

// Rips every audio track of `fake` and returns the AccurateRip checksums.
std::vector<std::pair<uint32_t, uint32_t>> ripAccurateRip(FakeDrive& fake, int offset) {
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    cdr::RipOptions options;
    options.readOffsetSamples = offset;
    cdr::Ripper ripper(drive, toc, options);
    std::vector<std::pair<uint32_t, uint32_t>> sums;
    for (const cdr::Track& t : toc.tracks) {
        if (!t.isAudio) continue;
        cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, t);
        ripper.ripTrack(t, [&](const uint8_t* p, size_t n) { ar.update(p, n); });
        CHECK_EQ(ar.samples(), t.lengthSectors * cdr::kSamplesPerSector);
        sums.emplace_back(ar.v1(), ar.v2());
    }
    return sums;
}

}  // namespace

TEST(accuraterip_disc_ids_match_reference) {
    // Real discs (ARver test vectors; Enhanced CD ids as computed by EAC / dBpoweramp).
    struct Case {
        std::vector<uint32_t> lengths;
        uint32_t pregap;
        uint32_t data;
        const char* id;
    };
    const Case cases[] = {
        {{75258, 54815, 205880}, 0, 0, "003-00084264-001cc184-19117f03"},
        {{279037}, 0, 0, "001-000441fd-000883fb-020e8801"},
        {{107450, 71470, 105737, 71600}, 33, 0, "004-000e26d9-00380804-3e128e04"},
        {{143963}, 32, 0, "001-0002329b-00046516-02077f01"},
        {{12617, 27720, 22738, 30185, 24705, 33750, 32475, 30920, 32195, 22880}, 0, 52066,
         "010-00164419-00b9f6e2-9e11600b"},
        {{90778}, 0, 164721, "001-00041293-00082527-100de602"},
        {{14765, 14932, 12508, 525, 20937, 6025, 19753, 35570, 17777, 15258, 23515, 18512, 26168, 13440}, 12375, 0,
         "014-001ba337-01281b14-cf0c7b0e"},
    };
    for (const Case& c : cases) {
        const std::string id = cdr::AccurateRipDiscId::fromToc(tocFromLengths(c.lengths, c.pregap, c.data)).toString();
        if (id != c.id) std::fprintf(stderr, "  got %s, expected %s\n", id.c_str(), c.id);
        CHECK(id == c.id);
    }
}

TEST(accuraterip_disc_id_mixed_mode_skips_data_track) {
    // "Mortal Kombat Trilogy": data track 1 followed by 28 audio tracks.
    const uint32_t starts[] = {150,    66728,  76502,  85963,  93760,  104048, 116066, 124743, 134206, 142916,
                               151852, 160720, 169425, 178731, 188459, 196711, 206354, 214845, 223686, 225746,
                               226384, 232668, 239447, 239931, 244989, 254264, 260066, 261222, 261674};
    cdr::Toc toc;
    int number = 1;
    for (uint32_t s : starts) {
        cdr::Track t;
        t.number = number++;
        t.startLba = s - cdr::kPregapSectors;
        t.isAudio = t.number != 1;
        toc.tracks.push_back(t);
    }
    toc.leadOutLba = 261976 - cdr::kPregapSectors;
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(toc);
    CHECK(id.toString() == "028-00517a54-05a845d2-af0da31d");
    // The database slots follow the TOC: the first audio track is the 2nd entry.
    CHECK_EQ(cdr::accurateRipEntryIndex(toc, toc.tracks[1]), 1u);
}

TEST(accuraterip_disc_id_from_enhanced_cd_toc) {
    FakeDrive fake({{0, false}, {20000, false}, {50000, true}}, 60000);
    cdr::CdDrive drive(fake);
    const cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(drive.readToc());
    CHECK(id.toString() == "002-00013880-00035b61-26032003");
}

TEST(accuraterip_url) {
    cdr::AccurateRipDiscId id;
    id.audioTracks = 1;
    id.id1 = 0x000441fd;
    id.id2 = 0x000883fb;
    id.cddb = 0x020e8801;
    CHECK(id.url() ==
          "http://www.accuraterip.com/accuraterip/d/f/1/dBAR-001-000441fd-000883fb-020e8801.bin");
}

TEST(accuraterip_checksum_hand_computed) {
    // Middle track: every sample counts, weighted by its 1-based position.
    cdr::AccurateRipChecksum ones(1000, false, false);
    const std::vector<uint8_t> one = samplesToBytes(std::vector<uint32_t>(1000, 1));
    ones.update(one.data(), one.size());
    CHECK_EQ(ones.v1(), 1000u * 1001u / 2);
    CHECK_EQ(ones.v2(), 1000u * 1001u / 2);  // products never exceed 32 bits

    // k * 0xFFFFFFFF = (k - 1) << 32 | (2^32 - k): low parts -6, high parts 0+1+2.
    cdr::AccurateRipChecksum big(3, false, false);
    const std::vector<uint8_t> max = samplesToBytes({0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu});
    big.update(max.data(), max.size());
    CHECK_EQ(big.v1(), 0xFFFFFFFAu);
    CHECK_EQ(big.v2(), 0xFFFFFFFDu);

    // Little-endian sample layout: left channel in the low 16 bits.
    cdr::AccurateRipChecksum layout(2, false, false);
    const uint8_t pcm[] = {0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    layout.update(pcm, sizeof pcm);
    CHECK_EQ(layout.v1(), 0x00020001u + 2u * 0x01000000u);
}

TEST(accuraterip_checksum_skips_disc_edges) {
    const uint32_t n = 10000;
    const std::vector<uint8_t> pcm = samplesToBytes(std::vector<uint32_t>(n, 1));
    auto sum = [](uint64_t from, uint64_t to) { return uint32_t((from + to) * (to - from + 1) / 2); };

    cdr::AccurateRipChecksum first(n, true, false);  // samples 1..2939 are ignored
    first.update(pcm.data(), pcm.size());
    CHECK_EQ(first.v1(), sum(2940, n));

    cdr::AccurateRipChecksum last(n, false, true);  // the last 2940 samples are ignored
    last.update(pcm.data(), pcm.size());
    CHECK_EQ(last.v1(), sum(1, n - 2940));

    cdr::AccurateRipChecksum only(n, true, true);
    only.update(pcm.data(), pcm.size());
    CHECK_EQ(only.v1(), sum(2940, n - 2940));

    cdr::AccurateRipChecksum tiny(100, true, true);  // shorter than the skipped area
    tiny.update(pcm.data(), 400);
    CHECK_EQ(tiny.v1(), 0u);
    CHECK_EQ(tiny.v2(), 0u);
}

TEST(accuraterip_checksum_accepts_any_chunking) {
    const std::vector<uint8_t> pcm = expectedTrackData(450, 300);
    cdr::AccurateRipChecksum whole(300 * cdr::kSamplesPerSector, false, true);
    whole.update(pcm.data(), pcm.size());
    cdr::AccurateRipChecksum pieces(300 * cdr::kSamplesPerSector, false, true);
    for (size_t pos = 0, step = 1; pos < pcm.size(); pos += step, step = step % 13 + 1)
        pieces.update(pcm.data() + pos, std::min(step, pcm.size() - pos));
    CHECK_EQ(pieces.v1(), whole.v1());
    CHECK_EQ(pieces.v2(), whole.v2());
    CHECK_EQ(pieces.v1(), 0xe3ee9e80u);
    CHECK_EQ(pieces.v2(), 0xa44d0069u);
}

TEST(accuraterip_checksums_of_ripped_disc) {
    struct Case {
        int offset;
        uint32_t sums[3][2];
    };
    const Case cases[] = {
        {0, {{0xf3408d0eu, 0xc2c91b16u}, {0x81a4a0aau, 0xf5917daau}, {0xe3ee9e80u, 0xa44d0069u}}},
        {667, {{0x32ba416bu, 0x0242f822u}, {0xb9f72160u, 0x2de4a772u}, {0x3ed7b001u, 0xff3300b7u}}},
        {-30, {{0x40d6ed75u, 0x1061c380u}, {0xdaa17a10u, 0x4e8e8285u}, {0x7583966bu, 0x35e06f7fu}}},
    };
    for (const Case& c : cases) {
        FakeDrive fake = makeAudioDisc();
        const auto sums = ripAccurateRip(fake, c.offset);
        CHECK_EQ(sums.size(), 3u);
        for (size_t i = 0; i < sums.size() && i < 3; ++i) {
            CHECK_EQ(sums[i].first, c.sums[i][0]);
            CHECK_EQ(sums[i].second, c.sums[i][1]);
        }
    }
    FakeDrive single({{0, false}}, 300);  // first and last track at once
    const auto sums = ripAccurateRip(single, 0);
    CHECK_EQ(sums.size(), 1u);
    CHECK_EQ(sums[0].first, 0x4e62a386u);
    CHECK_EQ(sums[0].second, 0x0e99968du);
}

TEST(accuraterip_last_audio_track_of_enhanced_cd) {
    FakeDrive fake({{0, false}, {300, false}, {5000, true}}, 6000);
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    // Track 2 ends the audio session: its last 5 sectors are skipped.
    const cdr::Track& track = *toc.findTrack(2);
    const uint32_t samples = track.lengthSectors * cdr::kSamplesPerSector;
    const std::vector<uint8_t> pcm = expectedTrackData(300, track.lengthSectors);
    cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, track);
    cdr::AccurateRipChecksum last(samples, false, true);
    cdr::AccurateRipChecksum middle(samples, false, false);
    ar.update(pcm.data(), pcm.size());
    last.update(pcm.data(), pcm.size());
    middle.update(pcm.data(), pcm.size());
    CHECK_EQ(ar.v1(), last.v1());
    CHECK(ar.v1() != middle.v1());
}

TEST(accuraterip_parses_database_response) {
    std::vector<cdr::AccurateRipPressing> p = cdr::parseAccurateRipResponse(bytesToString(kDbar001));
    CHECK_EQ(p.size(), 2u);
    CHECK(p[0].id.toString() == "001-000441fd-000883fb-020e8801");
    CHECK(p[1].id == p[0].id);
    CHECK_EQ(p[0].tracks.size(), 1u);
    CHECK_EQ(p[0].tracks[0].confidence, 7);
    CHECK_EQ(p[0].tracks[0].checksum, 655774750u);
    CHECK_EQ(p[0].tracks[0].frame450Checksum, 0x07d5ca6bu);
    CHECK_EQ(p[1].tracks[0].confidence, 6);
    CHECK_EQ(p[1].tracks[0].checksum, 2150913158u);

    p = cdr::parseAccurateRipResponse(bytesToString(kDbar013));
    CHECK_EQ(p.size(), 1u);
    CHECK(p[0].id.toString() == "013-00206791-01486a82-a710de0d");
    CHECK_EQ(p[0].tracks.size(), 13u);
    CHECK_EQ(p[0].tracks[0].checksum, 3840044706u);
    CHECK_EQ(p[0].tracks[1].checksum, 3475065708u);
    CHECK_EQ(p[0].tracks[12].checksum, 0x8649e0deu);
    CHECK_EQ(p[0].tracks[12].confidence, 2);

    for (size_t cut : {size_t(5), size_t(13), size_t(50), kDbar013.size() - 1}) {
        bool threw = false;
        try {
            cdr::parseAccurateRipResponse(bytesToString(kDbar013).substr(0, cut));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK(cdr::parseAccurateRipResponse("").empty());
}

TEST(accuraterip_matching) {
    const std::vector<cdr::AccurateRipPressing> p = cdr::parseAccurateRipResponse(bytesToString(kDbar001));
    cdr::AccurateRipTrackResult r = cdr::matchAccurateRip(p, 0, 1, 655774750u, 1u);
    CHECK(r.accurate() && r.inDatabase());
    CHECK_EQ(r.v1Confidence, 7);
    CHECK_EQ(r.v2Confidence, 0);
    CHECK_EQ(r.totalConfidence, 13);
    CHECK(r.matchedVersion() == "v1");
    CHECK(r.describe() == "Accurately ripped with v1 (v2 0, v1 7 of 13 submissions; 1 of 2 pressings)");
    CHECK_EQ(r.pressings.size(), 2u);
    if (r.pressings.size() == 2) {
        CHECK_EQ(r.pressings[0].pressing, 1);
        CHECK_EQ(r.pressings[0].confidence, 7);
        CHECK_EQ(r.pressings[0].checksum, 655774750u);
        CHECK_EQ(r.pressings[0].version, 1);
        CHECK_EQ(r.pressings[1].pressing, 2);
        CHECK_EQ(r.pressings[1].confidence, 6);
        CHECK_EQ(r.pressings[1].version, 0);
    }

    r = cdr::matchAccurateRip(p, 0, 1, 1u, 2150913158u);
    CHECK_EQ(r.v2Confidence, 6);
    CHECK(r.matchedVersion() == "v2");
    CHECK(r.describe() == "Accurately ripped with v2 (v2 6, v1 0 of 13 submissions; 1 of 2 pressings)");
    CHECK(r.pressings.size() == 2 && r.pressings[1].version == 2 && r.pressings[0].version == 0);

    r = cdr::matchAccurateRip(p, 0, 1, 655774750u, 2150913158u);
    CHECK_EQ(r.confidence(), 13);
    CHECK_EQ(r.matchingPressings(), 2);
    CHECK(r.describe() == "Accurately ripped with v1+v2 (v2 6, v1 7 of 13 submissions; 2 of 2 pressings)");

    r = cdr::matchAccurateRip(p, 0, 1, 1u, 2u);
    CHECK(!r.accurate() && r.inDatabase());
    CHECK(r.matchedVersion().empty());
    CHECK(r.describe() == "Not accurate (v2 0, v1 0 of 13 submissions; 0 of 2 pressings)");

    r = cdr::matchAccurateRip(p, 1, 2, 1u, 2u);  // no entry (e.g. last track of a Mixed Mode CD)
    CHECK(!r.inDatabase());
    CHECK(r.describe() == "Not in database");

    // Zero-confidence placeholders never match, not even a silent track's 0.
    cdr::AccurateRipPressing placeholder;
    placeholder.tracks.resize(1);
    r = cdr::matchAccurateRip({placeholder}, 0, 1, 0u, 0u);
    CHECK(!r.inDatabase() && !r.accurate());
}

TEST(accuraterip_lookup) {
    cdr::AccurateRipDiscId id = cdr::AccurateRipDiscId::fromToc(tocFromLengths({279037}, 0, 0));
    CannedHttp http;
    http.response = httpReply(200, bytesToString(kDbar001));
    cdr::AccurateRipLookup l = cdr::lookupAccurateRip(http, id);
    CHECK(l.status == cdr::AccurateRipLookup::Status::Found);
    CHECK_EQ(l.pressings.size(), 2u);
    CHECK_EQ(http.requests.size(), 1u);
    CHECK(http.requests[0] == id.url());

    http.response = httpReply(404, "<html>Not Found</html>");
    l = cdr::lookupAccurateRip(http, id);
    CHECK(l.status == cdr::AccurateRipLookup::Status::NotFound);
    CHECK(l.pressings.empty());

    http.response = cdr::HttpResponse{};
    http.response.error = "no network";
    l = cdr::lookupAccurateRip(http, id);
    CHECK(l.status == cdr::AccurateRipLookup::Status::Error);
    CHECK(l.error == "no network");

    http.response = httpReply(500);
    CHECK(cdr::lookupAccurateRip(http, id).status == cdr::AccurateRipLookup::Status::Error);

    http.response = httpReply(200, bytesToString(kDbar013));  // another disc's record
    l = cdr::lookupAccurateRip(http, id);
    CHECK(l.status == cdr::AccurateRipLookup::Status::Error);
    CHECK(l.pressings.empty());

    http.response = httpReply(200, bytesToString(kDbar001).substr(0, 30));  // truncated
    CHECK(cdr::lookupAccurateRip(http, id).status == cdr::AccurateRipLookup::Status::Error);

    http.response = httpReply(200, "");
    CHECK(cdr::lookupAccurateRip(http, id).status == cdr::AccurateRipLookup::Status::Error);
}

TEST(accuraterip_offset_scan_matches_direct_checksums) {
    // The single-pass scan must equal ripping with each offset separately,
    // including the skipped edges of the first / last track and the silence
    // outside the disc.
    const uint32_t maxOffset = 700;
    for (int number = 1; number <= 3; ++number) {
        FakeDrive fake = makeAudioDisc();
        cdr::CdDrive drive(fake);
        cdr::Toc toc = drive.readToc();
        const cdr::Track& track = *toc.findTrack(number);
        const std::vector<uint32_t> sums =
            cdr::scanReadOffsets(drive, toc, track, maxOffset, {}).checksums();
        CHECK_EQ(sums.size(), size_t(2 * maxOffset + 1));
        for (int offset : {-700, -699, -588, -1, 0, 1, 6, 667, 699, 700}) {
            FakeDrive again = makeAudioDisc();
            OffsetRip rip = ripWithOffset(again, number, offset);
            cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, track);
            ar.update(rip.bytes.data(), rip.bytes.size());
            CHECK_EQ(sums[size_t(offset + int(maxOffset))], ar.v1());
        }
    }
}

TEST(accuraterip_offset_detection) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    const cdr::Track& track = *toc.findTrack(2);
    auto v1At = [&](int offset) {
        FakeDrive other = makeAudioDisc();
        OffsetRip rip = ripWithOffset(other, 2, offset);
        cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, track);
        ar.update(rip.bytes.data(), rip.bytes.size());
        return ar.v1();
    };
    // Two pressings: rips made with +48 (confidence 5) and -472 (confidence 2).
    std::vector<cdr::AccurateRipPressing> pressings(2);
    for (cdr::AccurateRipPressing& p : pressings) p.tracks.resize(3);
    pressings[0].tracks[1] = {5, v1At(48), 0};
    pressings[1].tracks[1] = {2, v1At(-472), 0};

    const cdr::AccurateRipOffsetScan scan = cdr::scanReadOffsets(drive, toc, track, 1000, {});
    const std::vector<cdr::AccurateRipOffsetMatch> found = cdr::findAccurateRipOffsets(scan, pressings, 1);
    CHECK_EQ(found.size(), 2u);
    if (found.size() == 2) {
        CHECK_EQ(found[0].offset, 48);
        CHECK_EQ(found[0].confidence(), 5);
        CHECK_EQ(found[0].v1Confidence, 5);
        CHECK(found[0].matchedVersion() == "v1");
        CHECK_EQ(found[1].offset, -472);
        CHECK_EQ(found[1].confidence(), 2);
    }
    CHECK(cdr::findAccurateRipOffsets(scan, pressings, 0).empty());
}

// Track checksums ripped separately at `offset` (reference for the scan).
static cdr::AccurateRipChecksum checksumAtOffset(const cdr::Toc& toc, int number, int offset) {
    FakeDrive fake = makeAudioDisc();
    OffsetRip rip = ripWithOffset(fake, number, offset);
    cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, *toc.findTrack(number));
    ar.update(rip.bytes.data(), rip.bytes.size());
    return ar;
}

TEST(accuraterip_offset_scan_v2_matches_direct_checksums) {
    const uint32_t maxOffset = 700;
    for (int number = 1; number <= 3; ++number) {
        FakeDrive fake = makeAudioDisc();
        cdr::CdDrive drive(fake);
        cdr::Toc toc = drive.readToc();
        const cdr::AccurateRipOffsetScan scan =
            cdr::scanReadOffsets(drive, toc, *toc.findTrack(number), maxOffset, {});
        const std::vector<uint32_t> residues = scan.v2Residues();
        CHECK_EQ(residues.size(), size_t(2 * maxOffset + 1));
        for (int offset : {-700, -699, -588, -1, 0, 1, 6, 667, 699, 700}) {
            const uint32_t v2 = checksumAtOffset(toc, number, offset).v2();
            CHECK_EQ(scan.v2Checksum(offset), v2);
            // The residue filter must never reject the true checksum.
            CHECK(scan.mayMatchV2(residues[size_t(offset + int(maxOffset))], v2));
        }
    }
}

TEST(accuraterip_v2_residue_filter_is_exact_and_selective) {
    // Every offset: the residue equals the exact sum mod 2^32 - 1, the true v2
    // always passes, and unrelated checksums are almost always rejected.
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    const uint32_t maxOffset = 300;
    const cdr::AccurateRipOffsetScan scan = cdr::scanReadOffsets(drive, toc, *toc.findTrack(2), maxOffset, {});
    const std::vector<uint32_t> residues = scan.v2Residues();
    int falsePositives = 0;
    for (size_t i = 0; i < residues.size(); ++i) {
        const int offset = int(i) - int(maxOffset);
        const uint32_t v2 = scan.v2Checksum(offset);
        CHECK(scan.mayMatchV2(residues[i], v2));
        falsePositives += scan.mayMatchV2(residues[i], v2 ^ 0x5A5A5A5Au) ? 1 : 0;
    }
    CHECK(falsePositives < 5);
}

TEST(accuraterip_offset_detection_with_v2) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    cdr::Toc toc = drive.readToc();
    // Pressing 1 only has v2 rips made at +48, pressing 2 v1 rips at +48,
    // pressing 3 v2 rips at -472.
    std::vector<cdr::AccurateRipPressing> pressings(3);
    for (cdr::AccurateRipPressing& p : pressings) p.tracks.resize(3);
    pressings[0].tracks[1] = {9, checksumAtOffset(toc, 2, 48).v2(), 0};
    pressings[1].tracks[1] = {4, checksumAtOffset(toc, 2, 48).v1(), 0};
    pressings[2].tracks[1] = {3, checksumAtOffset(toc, 2, -472).v2(), 0};

    const cdr::AccurateRipOffsetScan scan = cdr::scanReadOffsets(drive, toc, *toc.findTrack(2), 1000, {});
    const std::vector<cdr::AccurateRipOffsetMatch> found = cdr::findAccurateRipOffsets(scan, pressings, 1);
    CHECK_EQ(found.size(), 2u);
    if (found.size() == 2) {
        CHECK_EQ(found[0].offset, 48);
        CHECK_EQ(found[0].v2Confidence, 9);
        CHECK_EQ(found[0].v1Confidence, 4);
        CHECK_EQ(found[0].pressings, 2);
        CHECK(found[0].matchedVersion() == "v1+v2");
        CHECK_EQ(found[1].offset, -472);
        CHECK_EQ(found[1].v2Confidence, 3);
        CHECK_EQ(found[1].v1Confidence, 0);
        CHECK(found[1].matchedVersion() == "v2");
    }

    // A disc with v2 entries only (the case v1-only detection missed).
    pressings.erase(pressings.begin() + 1);
    const std::vector<cdr::AccurateRipOffsetMatch> v2Only = cdr::findAccurateRipOffsets(scan, pressings, 1);
    CHECK(!v2Only.empty() && v2Only[0].offset == 48 && v2Only[0].v1Confidence == 0);
}


// --- FLAC (#7) ------------------------------------------------------------------

namespace {

std::string md5Hex(const std::string& text) {
    cdr::Md5 md5;
    md5.update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const std::array<uint8_t, 16> d = md5.finish();
    std::string hex;
    char buf[3];
    for (uint8_t b : d) {
        std::snprintf(buf, sizeof buf, "%02x", b);
        hex += buf;
    }
    return hex;
}

std::string bitString(const cdr::flac::BitWriter& w) {
    std::string s;
    for (uint8_t b : w.bytes())
        for (int i = 7; i >= 0; --i) s += char('0' + ((b >> i) & 1));
    return s;
}

std::vector<uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Encodes `pcm` with FlacWriter (in chunks of `chunk` bytes) and returns the file contents.
std::vector<uint8_t> encodeFlac(const std::vector<uint8_t>& pcm, const cdr::TrackMetadata& meta = {},
                                size_t chunk = cdr::kSectorBytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_test.flac";
    {
        cdr::FlacWriter flac;
        flac.open(path, meta);
        for (size_t pos = 0; pos < pcm.size(); pos += chunk)
            flac.write(pcm.data() + pos, std::min(chunk, pcm.size() - pos));
        flac.close();
        CHECK_EQ(flac.totalSamples(), uint64_t(pcm.size() / 4));
    }
    std::vector<uint8_t> file = readAll(path);
    std::filesystem::remove(path);
    return file;
}

std::array<uint8_t, 16> md5Of(const std::vector<uint8_t>& data) {
    cdr::Md5 md5;
    md5.update(data.data(), data.size());
    return md5.finish();
}

}  // namespace

TEST(md5_known_vectors) {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
    // Incremental updates across the 64-byte block boundary give the same digest.
    const std::string text(200, 'x');
    for (size_t split : {size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(130)}) {
        cdr::Md5 a;
        a.update(reinterpret_cast<const uint8_t*>(text.data()), split);
        a.update(reinterpret_cast<const uint8_t*>(text.data()) + split, text.size() - split);
        CHECK(a.finish() == md5Of(std::vector<uint8_t>(text.begin(), text.end())));
    }
}

TEST(flac_crc_known_vectors) {
    const uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(cdr::flac::crc8(text, sizeof text), 0xF4);     // CRC-8 (poly 0x07)
    CHECK_EQ(cdr::flac::crc16(text, sizeof text), 0xFEE8);  // CRC-16/BUYPASS (poly 0x8005)
    CHECK_EQ(cdr::flac::crc8(text, 0), 0);
}

TEST(flac_bit_writer) {
    cdr::flac::BitWriter w;
    w.writeBits(0x5, 3);         // 101
    w.writeSigned(-2, 4);        // 1110
    w.writeUnary(3);             // 0001
    w.writeBits(0xFFFFFFFFu, 32);
    w.writeBits(0x1FF, 2);       // only the low bits are used: 11
    CHECK_EQ(w.bitCount(), 45u);
    w.alignToByte();
    CHECK(bitString(w) == "101111000011111111111111111111111111111111111000");

    cdr::flac::BitWriter a, b;
    a.writeBits(1, 3);  // 001
    b.writeBits(0x2D, 7);  // 0101101
    b.writeUnary(40);
    a.append(b);
    CHECK_EQ(a.bitCount(), 3u + 7u + 41u);
    a.alignToByte();
    CHECK(bitString(a) == "0010101101" + std::string(40, '0') + "1" + "00000");
}

TEST(flac_utf8_frame_numbers) {
    auto utf8 = [](uint32_t v) {
        cdr::flac::BitWriter w;
        w.writeUtf8(v);
        return w.bytes();
    };
    CHECK(utf8(0) == std::vector<uint8_t>({0x00}));
    CHECK(utf8(0x7F) == std::vector<uint8_t>({0x7F}));
    CHECK(utf8(0x80) == std::vector<uint8_t>({0xC2, 0x80}));
    CHECK(utf8(0x7FF) == std::vector<uint8_t>({0xDF, 0xBF}));
    CHECK(utf8(0x800) == std::vector<uint8_t>({0xE0, 0xA0, 0x80}));
    CHECK(utf8(0xFFFF) == std::vector<uint8_t>({0xEF, 0xBF, 0xBF}));
    CHECK(utf8(0x10000) == std::vector<uint8_t>({0xF0, 0x90, 0x80, 0x80}));
    CHECK(utf8(0x7FFFFFFF) == std::vector<uint8_t>({0xFD, 0xBF, 0xBF, 0xBF, 0xBF, 0xBF}));
}

TEST(flac_rice_coding) {
    CHECK_EQ(cdr::flac::zigzag(0), 0u);
    CHECK_EQ(cdr::flac::zigzag(-1), 1u);
    CHECK_EQ(cdr::flac::zigzag(1), 2u);
    CHECK_EQ(cdr::flac::zigzag(-3), 5u);
    CHECK_EQ(cdr::flac::zigzag(INT32_MIN), 0xFFFFFFFFu);
    cdr::flac::BitWriter w;
    w.writeRice(-3, 2);  // u = 5: quotient 1 -> "01", remainder "01"
    w.writeRice(0, 0);   // "1"
    w.writeRice(5, 1);   // u = 10: quotient 5 -> "000001", remainder "0"
    w.writeRice(-1, 3);  // u = 1: "1" + "001"
    w.alignToByte();
    CHECK(bitString(w) == "0101" "1" "0000010" "1001");  // 16 bits, already aligned
}

TEST(flac_vorbis_comment) {
    cdr::TrackMetadata m;
    m.trackNumber = 3;
    m.trackTotal = 12;
    m.title = "曲名";
    m.artist = "Artist";
    m.discId = "0A0B0C03";
    const std::vector<uint8_t> v = cdr::flac::vorbisComment(m, "vendor");
    const std::vector<uint8_t> expected = [] {
        std::vector<uint8_t> e;
        auto str = [&](const std::string& s) {
            for (int i = 0; i < 4; ++i) e.push_back(uint8_t(s.size() >> (8 * i)));
            e.insert(e.end(), s.begin(), s.end());
        };
        str("vendor");
        e.insert(e.end(), {5, 0, 0, 0});
        for (const char* f : {"TITLE=曲名", "ARTIST=Artist", "TRACKNUMBER=3", "TRACKTOTAL=12", "CDDB=0A0B0C03"}) str(f);
        return e;
    }();
    CHECK(v == expected);
}

TEST(flac_writer_produces_valid_stream) {
    const testsig::Signal music = testsig::music(4096 * 25 + 1000);
    cdr::TrackMetadata meta;
    meta.title = "Song";
    meta.artist = "Artist";
    meta.album = "Album";
    meta.albumArtist = "Album Artist";
    meta.genre = "Rock";
    meta.year = "1999";
    meta.trackNumber = 2;
    meta.trackTotal = 9;
    meta.discId = "12345678";
    const std::vector<uint8_t> file = encodeFlac(music.pcm, meta, 1000);  // chunks split samples
    const DecodedFlac d = decodeFlac(file);

    CHECK_EQ(d.minBlockSize, 4096u);
    CHECK_EQ(d.maxBlockSize, 4096u);
    CHECK_EQ(d.sampleRate, 44100u);
    CHECK_EQ(d.channels, 2u);
    CHECK_EQ(d.bitsPerSample, 16u);
    CHECK_EQ(d.totalSamples, uint64_t(4096 * 25 + 1000));
    CHECK(d.md5 == md5Of(music.pcm));
    CHECK(d.pcm == music.pcm);
    CHECK_EQ(d.frames, 26u);
    // Frame size bounds match the actual frames.
    std::vector<uint64_t> ends(d.frameOffsets.begin() + 1, d.frameOffsets.end());
    ends.push_back(file.size() - d.firstFrame);
    uint32_t minFrame = UINT32_MAX, maxFrame = 0;
    for (size_t i = 0; i < ends.size(); ++i) {
        const uint32_t size = uint32_t(ends[i] - d.frameOffsets[i]);
        maxFrame = std::max(maxFrame, size);
        if (i + 1 < ends.size()) minFrame = std::min(minFrame, size);  // the short last frame is smaller
    }
    CHECK_EQ(d.maxFrameSize, maxFrame);
    CHECK(d.minFrameSize <= minFrame);

    // STREAMINFO, VORBIS_COMMENT, SEEKTABLE, PADDING (last).
    CHECK(d.blockTypes == std::vector<int>({0, 4, 3, 1}));
    CHECK(d.vendor.rfind("cdreader ", 0) == 0);
    CHECK(d.comments == std::vector<std::string>({"TITLE=Song", "ARTIST=Artist", "ALBUM=Album",
                                                  "ALBUMARTIST=Album Artist", "TRACKNUMBER=2", "TRACKTOTAL=9",
                                                  "DATE=1999", "GENRE=Rock", "CDDB=12345678"}));
    // 2.4 s of audio: a seek point at 0 s only (one every 10 s), on the first frame.
    CHECK_EQ(d.seekPoints.size(), 1u);
    CHECK(d.seekPoints[0].sample == 0 && d.seekPoints[0].offset == 0 && d.seekPoints[0].samples == 4096);

    // First frame header: sync, block size 4096 (code 12), 44.1 kHz (code 9), 16 bit, frame 0.
    const uint8_t* h = file.data() + d.firstFrame;
    CHECK(h[0] == 0xFF && h[1] == 0xF8 && h[2] == 0xC9 && (h[3] & 0x0F) == 0x08 && h[4] == 0x00);
    CHECK_EQ(h[5], cdr::flac::crc8(h, 5));
    // Last frame: 1000 samples, coded as a 16-bit "block size - 1" field.
    const uint8_t* last = h + d.frameOffsets.back();
    CHECK(last[0] == 0xFF && last[1] == 0xF8 && last[2] == 0x79 && last[4] == 25 && last[5] == 0x03 && last[6] == 0xE7);
}

TEST(flac_round_trip_synthetic_signals) {
    for (const testsig::Signal& s : testsig::all()) {
        const DecodedFlac d = decodeFlac(encodeFlac(s.pcm));
        const bool ok = d.pcm == s.pcm && d.md5 == md5Of(s.pcm) && d.totalSamples == s.pcm.size() / 4;
        if (!ok) std::fprintf(stderr, "  round trip failed: %s\n", s.name.c_str());
        CHECK(ok);
    }
}

TEST(flac_long_track_seek_table) {
    // 25 s of audio: seek points at 0, 10 and 20 s, each on the frame containing that sample.
    const testsig::Signal s = testsig::make("ramp", 44100 * 25, [](size_t i, int16_t& l, int16_t& r) {
        l = int16_t(i % 2000);
        r = int16_t(-int(i % 3001));
    });
    const DecodedFlac d = decodeFlac(encodeFlac(s.pcm, {}, 65536));
    CHECK(d.pcm == s.pcm);
    CHECK_EQ(d.seekPoints.size(), 3u);
    for (size_t i = 0; i < d.seekPoints.size() && i < 3; ++i) {
        const uint64_t frame = 441000 * i / 4096;
        CHECK_EQ(d.seekPoints[i].sample, frame * 4096);
        CHECK_EQ(d.seekPoints[i].offset, d.frameOffsets[size_t(frame)]);
        CHECK_EQ(d.seekPoints[i].samples, 4096u);
    }
}

TEST(flac_encoder_picks_cheap_representations) {
    cdr::flac::FrameEncoder encoder;
    std::vector<int32_t> zeros(4096, 0), ramp(4096), noise(4096);
    testsig::Noise n(9);
    for (size_t i = 0; i < 4096; ++i) {
        ramp[i] = int32_t(i) - 2048;
        noise[i] = int32_t(n.next() * 30000);
    }
    // Silence: two CONSTANT subframes -> 6 header bytes + 2 * 3 bytes + CRC-16.
    std::vector<uint8_t> frame = encoder.encode(zeros.data(), zeros.data(), 4096, 0);
    CHECK_EQ(frame.size(), 14u);
    // A linear ramp is predicted exactly by FIXED order 2 (residual all zero).
    frame = encoder.encode(ramp.data(), ramp.data(), 4096, 1);
    CHECK(frame.size() < 40);
    CHECK_EQ(frame[3] >> 4, 8);  // identical channels -> left/side (side is constant 0)
    // Noise cannot be compressed: VERBATIM bounds the size.
    frame = encoder.encode(noise.data(), zeros.data(), 4096, 2);
    CHECK(frame.size() <= 4096u * 2 + 20);
}

TEST(flac_writer_rejects_partial_sample) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_partial.flac";
    bool threw = false;
    {
        cdr::FlacWriter flac;
        flac.open(path, {});
        const uint8_t pcm[6] = {1, 2, 3, 4, 5, 6};
        flac.write(pcm, sizeof pcm);
        try {
            flac.close();
        } catch (const std::runtime_error&) {
            threw = true;
        }
    }
    std::filesystem::remove(path);
    CHECK(threw);
}

// --- Tags, WAV metadata, CUE sheets, single-file rips, Ogg -----------------

namespace {

uint32_t le32At(const std::vector<uint8_t>& v, size_t o) {
    return uint32_t(v[o]) | uint32_t(v[o + 1]) << 8 | uint32_t(v[o + 2]) << 16 | uint32_t(v[o + 3]) << 24;
}

std::string str(const std::vector<uint8_t>& v, size_t o, size_t n) {
    return std::string(reinterpret_cast<const char*>(v.data() + o), n);
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Set CDREADER_TEST_OUTPUT to a directory to keep sample files for checking
// with external tools (ffprobe, MediaInfo, ExifTool, ogginfo, ...).
void keepSample(const std::string& name, const std::vector<uint8_t>& bytes) {
    const char* dir = std::getenv("CDREADER_TEST_OUTPUT");
    if (!dir || !*dir) return;
    std::ofstream out(std::filesystem::u8path(dir) / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

cdr::AlbumMetadata sampleAlbum() {
    cdr::AlbumMetadata album;
    album.artist = "Artist";
    album.title = "Album";
    album.genre = "Hard Rock";
    album.year = "1999";
    album.discId = "0A0B0C03";
    album.trackTitles = {"One", "Two \"2\"", "Three"};
    album.trackArtists = {"", "Guest", ""};
    return album;
}

struct Chunk {
    std::string id;
    size_t offset;  // of the payload
    uint32_t size;
};

// Walks the chunks of a RIFF file and checks that they exactly fill it.
std::vector<Chunk> riffChunks(const std::vector<uint8_t>& file) {
    std::vector<Chunk> chunks;
    CHECK(file.size() >= 12 && str(file, 0, 4) == "RIFF" && str(file, 8, 4) == "WAVE");
    CHECK_EQ(le32At(file, 4), uint32_t(file.size() - 8));
    size_t pos = 12;
    while (pos + 8 <= file.size()) {
        Chunk c{str(file, pos, 4), pos + 8, le32At(file, pos + 4)};
        chunks.push_back(c);
        pos = c.offset + c.size + (c.size % 2);
    }
    CHECK_EQ(pos, file.size());
    return chunks;
}

// Sub-chunks of a LIST/INFO payload as id -> text (without the terminator).
std::vector<std::pair<std::string, std::string>> infoFields(const std::vector<uint8_t>& v, size_t offset, uint32_t size) {
    std::vector<std::pair<std::string, std::string>> fields;
    CHECK(str(v, offset, 4) == "INFO");
    size_t pos = offset + 4;
    while (pos + 8 <= offset + size) {
        const uint32_t n = le32At(v, pos + 4);
        CHECK(n > 0 && v[pos + 8 + n - 1] == 0);  // NUL-terminated
        fields.emplace_back(str(v, pos, 4), str(v, pos + 8, n - 1));
        pos += 8 + n + (n % 2);
    }
    CHECK_EQ(pos, offset + size);
    return fields;
}

uint32_t syncsafe(const std::vector<uint8_t>& v, size_t o) {
    return uint32_t(v[o]) << 21 | uint32_t(v[o + 1]) << 14 | uint32_t(v[o + 2]) << 7 | v[o + 3];
}

// Frames of an ID3v2.4 tag as id -> payload (after the encoding byte).
std::vector<std::pair<std::string, std::string>> id3Frames(const std::vector<uint8_t>& v, size_t offset) {
    std::vector<std::pair<std::string, std::string>> frames;
    CHECK(str(v, offset, 3) == "ID3" && v[offset + 3] == 4 && v[offset + 4] == 0 && v[offset + 5] == 0);
    const size_t end = offset + 10 + syncsafe(v, offset + 6);
    size_t pos = offset + 10;
    while (pos + 10 <= end) {
        const uint32_t n = syncsafe(v, pos + 4);
        CHECK_EQ(v[pos + 10], 3);  // UTF-8
        frames.emplace_back(str(v, pos, 4), str(v, pos + 11, n - 1));
        pos += 10 + n;
    }
    CHECK_EQ(pos, end);
    return frames;
}

std::string fieldValue(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& id) {
    for (const auto& [k, v] : fields)
        if (k == id) return v;
    return "<missing>";
}

struct OggPage {
    uint8_t flags;
    int64_t granule;
    uint32_t serial;
    uint32_t sequence;
    std::vector<uint8_t> lacing;
    std::vector<uint8_t> body;
};

// Splits an Ogg stream into pages, verifying the capture pattern and CRC.
std::vector<OggPage> parseOgg(const std::vector<uint8_t>& s) {
    std::vector<OggPage> pages;
    size_t pos = 0;
    while (pos < s.size()) {
        CHECK(pos + 27 <= s.size() && str(s, pos, 4) == "OggS" && s[pos + 4] == 0);
        OggPage p;
        p.flags = s[pos + 5];
        uint64_t g = 0;
        for (int i = 7; i >= 0; --i) g = (g << 8) | s[pos + 6 + size_t(i)];
        p.granule = int64_t(g);
        p.serial = le32At(s, pos + 14);
        p.sequence = le32At(s, pos + 18);
        const size_t segments = s[pos + 26];
        p.lacing.assign(s.begin() + ptrdiff_t(pos + 27), s.begin() + ptrdiff_t(pos + 27 + segments));
        size_t bodySize = 0;
        for (uint8_t l : p.lacing) bodySize += l;
        const size_t pageSize = 27 + segments + bodySize;
        CHECK(pos + pageSize <= s.size());
        p.body.assign(s.begin() + ptrdiff_t(pos + 27 + segments), s.begin() + ptrdiff_t(pos + pageSize));
        std::vector<uint8_t> copy(s.begin() + ptrdiff_t(pos), s.begin() + ptrdiff_t(pos + pageSize));
        const uint32_t stored = le32At(copy, 22);
        std::fill(copy.begin() + 22, copy.begin() + 26, uint8_t(0));
        CHECK_EQ(cdr::oggCrc32(copy.data(), copy.size()), stored);
        pages.push_back(std::move(p));
        pos += pageSize;
    }
    return pages;
}

// Reassembles packets from pages (checking the continued flags).
std::vector<std::vector<uint8_t>> oggPackets(const std::vector<OggPage>& pages) {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> current;
    bool open = false;
    for (const OggPage& p : pages) {
        CHECK_EQ(bool(p.flags & 0x01), open);
        size_t pos = 0;
        for (uint8_t l : p.lacing) {
            current.insert(current.end(), p.body.begin() + ptrdiff_t(pos), p.body.begin() + ptrdiff_t(pos + l));
            pos += l;
            open = l == 255;
            if (!open) {
                packets.push_back(current);
                current.clear();
            }
        }
    }
    CHECK(!open);
    return packets;
}

}  // namespace

TEST(riff_info_chunk_layout) {
    cdr::TrackMetadata m = sampleAlbum().forTrack(2, 3);
    m.title = "Two";  // 3 bytes + NUL: even, no pad
    const std::vector<uint8_t> c = cdr::riffInfoChunk(m);
    CHECK(str(c, 0, 4) == "LIST");
    CHECK_EQ(le32At(c, 4), uint32_t(c.size() - 8));
    CHECK_EQ(c.size() % 2, 0u);
    const auto f = infoFields(c, 8, le32At(c, 4));
    CHECK(fieldValue(f, "INAM") == "Two");
    CHECK(fieldValue(f, "IART") == "Guest");
    CHECK(fieldValue(f, "IPRD") == "Album");
    CHECK(fieldValue(f, "ITRK") == "2");
    CHECK(fieldValue(f, "ICRD") == "1999");
    CHECK(fieldValue(f, "IGNR") == "Hard Rock");
    CHECK(fieldValue(f, "ICMT") == "CDDB disc ID 0A0B0C03");
    // "Artist" is 6 bytes + NUL = 7: the size says 7 and a pad byte follows.
    m.artist = "Artist";
    const std::vector<uint8_t> c2 = cdr::riffInfoChunk(m);
    CHECK(fieldValue(infoFields(c2, 8, le32At(c2, 4)), "IART") == "Artist");
}

TEST(tags_skipped_without_metadata) {
    CHECK(cdr::riffInfoChunk(cdr::TrackMetadata{}).empty());
    CHECK(cdr::id3v2Tag(cdr::TrackMetadata{}).empty());
}

TEST(id3v2_tag_frames) {
    cdr::TrackMetadata m = sampleAlbum().forTrack(2, 3);
    m.title = std::string(200, 'x');  // frame size above 127 needs the syncsafe encoding
    const std::vector<uint8_t> tag = cdr::id3v2Tag(m);
    CHECK_EQ(size_t(syncsafe(tag, 6)) + 10, tag.size());
    for (size_t i = 6; i < 10; ++i) CHECK(tag[i] < 0x80);
    const auto f = id3Frames(tag, 0);
    CHECK(fieldValue(f, "TIT2") == m.title);
    CHECK(fieldValue(f, "TPE1") == "Guest");
    CHECK(fieldValue(f, "TALB") == "Album");
    CHECK(fieldValue(f, "TPE2") == "Artist");
    CHECK(fieldValue(f, "TRCK") == "2/3");
    CHECK(fieldValue(f, "TDRC") == "1999");
    CHECK(fieldValue(f, "TCON") == "Hard Rock");
    CHECK(fieldValue(f, "TXXX") == std::string("DISCID") + '\0' + "0A0B0C03");
}

TEST(wav_writer_writes_tags_after_data) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_test_tags.wav";
    cdr::TrackMetadata m = sampleAlbum().forTrack(1, 3);
    m.title = "日本語のタイトル";
    for (size_t dataSize : {size_t(2 * cdr::kSectorBytes), size_t(1001)}) {  // odd size needs a pad byte
        const std::vector<uint8_t> pcm = expectedTrackData(10, 2);
        {
            cdr::WavWriter wav;
            wav.open(path, m);
            wav.write(pcm.data(), dataSize);
            wav.close();
        }
        const std::vector<uint8_t> file = readFile(path);
        std::filesystem::remove(path);
        if (dataSize % 2 == 0) keepSample("tagged.wav", file);

        const std::vector<Chunk> chunks = riffChunks(file);
        CHECK_EQ(chunks.size(), 4u);
        if (chunks.size() != 4) continue;
        CHECK(chunks[0].id == "fmt " && chunks[0].size == 16 && chunks[0].offset == 20);
        CHECK(chunks[1].id == "data" && chunks[1].offset == 44 && chunks[1].size == dataSize);
        CHECK(std::equal(pcm.begin(), pcm.begin() + ptrdiff_t(dataSize), file.begin() + 44));
        CHECK(chunks[2].id == "LIST");
        CHECK(chunks[2].offset % 2 == 0);
        const auto info = infoFields(file, chunks[2].offset, chunks[2].size);
        CHECK(fieldValue(info, "INAM") == m.title);
        CHECK(fieldValue(info, "ITRK") == "1");
        CHECK(chunks[3].id == "id3 ");
        const auto id3 = id3Frames(file, chunks[3].offset);
        CHECK(fieldValue(id3, "TIT2") == m.title);
        CHECK(fieldValue(id3, "TRCK") == "1/3");
    }
}

TEST(cue_sheet_single_file) {
    std::vector<cdr::Track> tracks = {{1, 0, 300}, {2, 300, 150}, {3, 450, 300 + 75 * 60 * 2 + 7}};
    tracks[1].preEmphasis = true;
    tracks[2].copyPermitted = true;
    tracks[2].preEmphasis = true;
    const cdr::AlbumMetadata album = sampleAlbum();
    const std::string cue = cdr::formatCueSheet(album, cdr::singleFileCueTracks(tracks, "Artist - Album.wav", album));
    const std::string expected =
        "REM COMMENT \"cdreader\"\r\n"
        "REM GENRE \"Hard Rock\"\r\n"
        "REM DATE 1999\r\n"
        "REM DISCID 0A0B0C03\r\n"
        "PERFORMER \"Artist\"\r\n"
        "TITLE \"Album\"\r\n"
        "FILE \"Artist - Album.wav\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE \"One\"\r\n"
        "    PERFORMER \"Artist\"\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"Two '2'\"\r\n"
        "    PERFORMER \"Guest\"\r\n"
        "    FLAGS PRE\r\n"
        "    INDEX 01 00:04:00\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    TITLE \"Three\"\r\n"
        "    PERFORMER \"Artist\"\r\n"
        "    FLAGS DCP PRE\r\n"
        "    INDEX 01 00:06:00\r\n";
    CHECK(cue == expected);
    keepSample("ascii.cue", std::vector<uint8_t>(cue.begin(), cue.end()));
}

TEST(cue_sheet_rejects_non_adjacent_tracks) {
    const std::vector<cdr::Track> tracks = {{1, 0, 300}, {3, 450, 300}};
    bool threw = false;
    try {
        cdr::singleFileCueTracks(tracks, "x.wav", {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    // A data track in between (mixed mode / CD-Extra layout) is a gap too.
    const std::vector<cdr::Track> gap = {{1, 0, 300}, {2, 300 + cdr::kSessionGapSectors, 300}};
    threw = false;
    try {
        cdr::singleFileCueTracks(gap, "x.wav", {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(cue_sheet_per_track_and_encoding) {
    const std::vector<cdr::Track> tracks = {{1, 0, 300}, {2, 300, 150}};
    cdr::AlbumMetadata album;  // nothing known: only the comment line and tracks
    std::string cue = cdr::formatCueSheet(album, cdr::perTrackCueTracks(tracks, {"Track01.flac", "Track02.flac"}, album));
    CHECK(cue ==
          "REM COMMENT \"cdreader\"\r\n"
          "FILE \"Track01.flac\" WAVE\r\n"
          "  TRACK 01 AUDIO\r\n"
          "    INDEX 01 00:00:00\r\n"
          "FILE \"Track02.flac\" WAVE\r\n"
          "  TRACK 02 AUDIO\r\n"
          "    INDEX 01 00:00:00\r\n");
    CHECK(cue.compare(0, 3, "\xEF\xBB\xBF") != 0);  // ASCII only: no BOM

    album.title = "アルバム\r\nTITLE \"injected\"";
    cue = cdr::formatCueSheet(album, cdr::singleFileCueTracks(tracks, "a.wav", album));
    CHECK(cue.compare(0, 3, "\xEF\xBB\xBF") == 0);
    CHECK(cue.find("TITLE \"アルバム  TITLE 'injected'\"\r\n") != std::string::npos);
    keepSample("utf8.cue", std::vector<uint8_t>(cue.begin(), cue.end()));

    CHECK(cdr::cueFileType("WAV") == "WAVE");
    CHECK(cdr::cueFileType("mp3") == "MP3");
    CHECK(cdr::cueFileType("aiff") == "AIFF");
    CHECK(cdr::formatCueTime(0) == "00:00:00");
    CHECK(cdr::formatCueTime(75 * 60 * 100 + 75 * 59 + 74) == "100:59:74");
}

// With read offset correction every track is cut from the drive's data
// shifted by the same offset, so tracks ripped back to back must join without
// gaps or overlaps: the image equals the shifted disc.
TEST(single_file_rip_is_contiguous_with_offset) {
    for (int offset : {0, 6, -472, 667, 1206}) {
        FakeDrive fake = makeAudioDisc();
        cdr::CdDrive drive(fake);
        const cdr::Toc toc = drive.readToc();
        cdr::RipOptions options;
        options.readOffsetSamples = offset;
        cdr::Ripper ripper(drive, toc, options);

        const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_test_image.wav";
        std::vector<uint8_t> concatenated;
        {
            cdr::WavWriter image;
            image.open(path, cdr::AlbumMetadata{}.forTrack(0, 3));
            for (const cdr::Track& t : toc.tracks) {
                Collected single;
                ripper.ripTrack(t, [&](const uint8_t* p, size_t n) {
                    image.write(p, n);
                    single.bytes.insert(single.bytes.end(), p, p + n);
                });
                concatenated.insert(concatenated.end(), single.bytes.begin(), single.bytes.end());
            }
            image.close();
        }
        const std::vector<uint8_t> file = readFile(path);
        std::filesystem::remove(path);

        const std::vector<uint8_t> expected = expectedWithOffset(0, 750, offset, 0, 750);
        CHECK(concatenated == expected);
        CHECK_EQ(file.size(), 44u + expected.size());
        CHECK(std::equal(expected.begin(), expected.end(), file.begin() + 44));
        // ...and each track's INDEX 01 is where that track's data starts in the file.
        const auto cue = cdr::singleFileCueTracks(toc.tracks, "image.wav", {});
        for (size_t i = 0; i < cue.size(); ++i) {
            const size_t at = size_t(cue[i].startSectors) * cdr::kSectorBytes;
            const OffsetRip alone = ripWithOffset(fake, cue[i].number, offset);
            CHECK(std::equal(alone.bytes.begin(), alone.bytes.end(), file.begin() + 44 + ptrdiff_t(at)));
        }
    }
}

TEST(album_file_base_names) {
    cdr::AlbumMetadata album;
    CHECK(cdr::albumFileBase(album, "CDImage") == "CDImage");
    album.title = "Album";
    CHECK(cdr::albumFileBase(album, "CDImage") == "Album");
    album.artist = "A/B";
    CHECK(cdr::albumFileBase(album, "CDImage") == "A_B - Album");
    album.artist = "Album";  // self-titled: no "Album - Album"
    CHECK(cdr::albumFileBase(album, "CDImage") == "Album");
}

TEST(ogg_crc_check_value) {
    const char* text = "123456789";
    // CRC-32 with polynomial 0x04C11DB7, init 0, no reflection, no final XOR.
    CHECK_EQ(cdr::oggCrc32(reinterpret_cast<const uint8_t*>(text), 9), 0x89A1897Fu);
    const uint32_t part = cdr::oggCrc32(reinterpret_cast<const uint8_t*>(text), 4);
    CHECK_EQ(cdr::oggCrc32(reinterpret_cast<const uint8_t*>(text) + 4, 5, part), 0x89A1897Fu);
}

TEST(ogg_single_page_known_bytes) {
    std::vector<uint8_t> out;
    cdr::OggStreamWriter ogg(0x12345678, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
    ogg.writePacket(std::vector<uint8_t>{'a', 'b', 'c'}, 0, true);
    // Computed independently (Python reference implementation of RFC 3533).
    const std::vector<uint8_t> expected = {0x4F, 0x67, 0x67, 0x53, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
                                           0x1A, 0xAD, 0x3C, 0x30, 0x01, 0x03, 0x61, 0x62, 0x63};
    CHECK(out == expected);
    CHECK(ogg.finished());
    bool threw = false;
    try {
        ogg.writePacket(std::vector<uint8_t>{1}, 1);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(ogg_lacing_spanning_and_flags) {
    std::vector<uint8_t> out;
    cdr::OggStreamWriter ogg(7, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
    std::vector<std::vector<uint8_t>> packets;
    for (size_t size : {size_t(19), size_t(0), size_t(254), size_t(255), size_t(256), size_t(510), size_t(200000),
                        size_t(3), size_t(255 * 255), size_t(100)}) {
        std::vector<uint8_t> p(size);
        for (size_t i = 0; i < size; ++i) p[i] = uint8_t(i * 31 + packets.size());
        packets.push_back(p);
    }
    ogg.writePacket(packets[0], 0);
    ogg.flush();  // a header packet alone on the first page
    ogg.flush();  // nothing buffered: no empty page
    for (size_t i = 1; i < packets.size(); ++i)
        ogg.writePacket(packets[i], int64_t(i * 1000), i + 1 == packets.size());
    keepSample("stream.ogg", out);

    const std::vector<OggPage> pages = parseOgg(out);
    CHECK(oggPackets(pages) == packets);
    CHECK_EQ(pages.size(), size_t(ogg.pagesWritten()));
    CHECK(pages.size() > 4);
    CHECK_EQ(pages[0].lacing.size(), 1u);
    CHECK_EQ(pages[0].flags, 0x02);  // BOS only
    CHECK_EQ(pages[0].granule, 0);
    int64_t lastGranule = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        const OggPage& p = pages[i];
        CHECK_EQ(p.serial, 7u);
        CHECK_EQ(p.sequence, uint32_t(i));
        CHECK(p.lacing.size() <= 255);
        CHECK_EQ(bool(p.flags & 0x02), i == 0);
        CHECK_EQ(bool(p.flags & 0x04), i + 1 == pages.size());
        // Granule: -1 when no packet ends on the page, else non-decreasing.
        const bool packetEnds = std::any_of(p.lacing.begin(), p.lacing.end(), [](uint8_t l) { return l < 255; });
        if (!packetEnds) {
            CHECK_EQ(p.granule, -1);
        } else {
            CHECK(p.granule >= lastGranule);
            lastGranule = p.granule;
        }
    }
    CHECK_EQ(pages.back().granule, 9000);
    // The 200000-byte packet spans several pages, so some page has only 255s
    // and continued pages must exist.
    CHECK(std::any_of(pages.begin(), pages.end(), [](const OggPage& p) { return p.granule == -1; }));
    CHECK(std::any_of(pages.begin(), pages.end(), [](const OggPage& p) { return p.flags & 0x01; }));
}

TEST(ogg_target_page_size) {
    int pages = 0;
    cdr::OggStreamWriter ogg(1, [&](const uint8_t*, size_t) { ++pages; }, 10);
    ogg.writePacket(std::vector<uint8_t>(9), 1);
    CHECK_EQ(pages, 0);
    ogg.writePacket(std::vector<uint8_t>(1), 2);  // body reaches 10 bytes
    CHECK_EQ(pages, 1);
}

TEST(ogg_eos_on_full_page) {
    // A packet of exactly 254 * 255 bytes uses 254 lacing values of 255 plus a
    // final 0: the page is full (255 values) when the packet ends.
    std::vector<uint8_t> out;
    cdr::OggStreamWriter ogg(1, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
    const std::vector<uint8_t> packet(254 * 255, 0x5A);
    ogg.writePacket(packet, 42, true);
    const std::vector<OggPage> pages = parseOgg(out);
    CHECK_EQ(pages.size(), 1u);
    CHECK_EQ(pages[0].lacing.size(), 255u);
    CHECK_EQ(pages[0].flags, 0x06);
    CHECK_EQ(pages[0].granule, 42);
}

// --- FLAC embedded CUE sheet (#16) ---------------------------------------------

namespace {

// Independent reader for the CUESHEET metadata block (FLAC format spec).
struct ParsedCueSheet {
    uint64_t leadIn = 0;
    bool isCd = false;
    struct Index {
        uint64_t offset;
        int number;
    };
    struct CueTrackEntry {
        uint64_t offset;
        int number;
        bool audio;
        bool preEmphasis;
        std::vector<Index> indexes;
    };
    std::vector<CueTrackEntry> tracks;
};

uint64_t be64At(const std::vector<uint8_t>& v, size_t o) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x = x << 8 | v[o + size_t(i)];
    return x;
}

// Returns the body of the first metadata block of `type`, or an empty vector.
std::vector<uint8_t> flacBlock(const std::vector<uint8_t>& file, int type) {
    size_t pos = 4;
    for (bool last = false; !last && pos + 4 <= file.size();) {
        last = (file[pos] & 0x80) != 0;
        const size_t length = size_t(file[pos + 1]) << 16 | size_t(file[pos + 2]) << 8 | file[pos + 3];
        if ((file[pos] & 0x7F) == type) return std::vector<uint8_t>(file.begin() + long(pos + 4), file.begin() + long(pos + 4 + length));
        pos += 4 + length;
    }
    return {};
}

ParsedCueSheet parseCueSheet(const std::vector<uint8_t>& b) {
    ParsedCueSheet c;
    size_t pos = 128;
    c.leadIn = be64At(b, pos);
    pos += 8;
    c.isCd = (b[pos] & 0x80) != 0;
    pos += 1 + 258;
    const int count = b[pos++];
    for (int t = 0; t < count; ++t) {
        ParsedCueSheet::CueTrackEntry e;
        e.offset = be64At(b, pos);
        e.number = b[pos + 8];
        e.audio = (b[pos + 21] & 0x80) == 0;
        e.preEmphasis = (b[pos + 21] & 0x40) != 0;
        const int indexes = b[pos + 35];
        pos += 36;
        for (int i = 0; i < indexes; ++i, pos += 12) e.indexes.push_back({be64At(b, pos), b[pos + 8]});
        c.tracks.push_back(e);
    }
    if (pos != b.size()) throw std::runtime_error("CUESHEET block has trailing bytes");
    return c;
}

cdr::EmbeddedCueSheet sampleEmbeddedCue() {
    cdr::EmbeddedCueSheet cue;
    cue.tracks = {{1, "Image.flac", 0, false, false, "One", ""},
                  {2, "Image.flac", 30, true, true, "Two", ""},
                  {3, "Image.flac", 75, false, false, "Three", ""}};
    cue.totalSectors = 100;
    cue.text = "\xEF\xBB\xBF" "FILE \"Image.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n";
    return cue;
}

}  // namespace

TEST(flac_cuesheet_block_layout) {
    const cdr::EmbeddedCueSheet cue = sampleEmbeddedCue();
    const std::vector<uint8_t> block = cdr::flac::cueSheet(cue, 100 * 588);
    CHECK_EQ(block.size(), size_t(396 + 3 * 48 + 36));
    CHECK(std::all_of(block.begin(), block.begin() + 128, [](uint8_t b) { return b == 0; }));  // no MCN
    const ParsedCueSheet c = parseCueSheet(block);
    CHECK(c.isCd);
    CHECK_EQ(c.leadIn, 88200u);
    CHECK_EQ(c.tracks.size(), 4u);
    if (c.tracks.size() == 4) {
        const uint64_t starts[] = {0, 30 * 588, 75 * 588};
        for (int i = 0; i < 3; ++i) {
            CHECK_EQ(c.tracks[size_t(i)].offset, starts[i]);
            CHECK_EQ(c.tracks[size_t(i)].number, i + 1);
            CHECK(c.tracks[size_t(i)].audio);
            CHECK_EQ(c.tracks[size_t(i)].indexes.size(), 1u);
            CHECK(c.tracks[size_t(i)].indexes[0].offset == 0 && c.tracks[size_t(i)].indexes[0].number == 1);
        }
        CHECK(c.tracks[1].preEmphasis && !c.tracks[0].preEmphasis);
        CHECK_EQ(c.tracks[3].number, 170);
        CHECK_EQ(c.tracks[3].offset, uint64_t(100 * 588));
        CHECK(c.tracks[3].indexes.empty());
    }
    CHECK_EQ(be64At(block, cdr::flac::cueSheetLeadOutOffsetPosition(3)), uint64_t(100 * 588));

    // Offsets that are not on CD frame boundaries: not marked as CD-DA.
    CHECK(!parseCueSheet(cdr::flac::cueSheet(cue, 100 * 588 + 1)).isCd);
    bool threw = false;
    try {
        cdr::flac::cueSheet(cue, 75 * 588);  // track 3 would start at the lead-out
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(flac_writer_embeds_cuesheet) {
    // 100 sectors of audio announced, 99 actually written: the lead-out
    // follows the real length.
    const testsig::Signal music = testsig::music(99 * 588);
    cdr::FlacWriter writer;
    CHECK(writer.canEmbedCueSheet());
    writer.setEmbeddedCueSheet(sampleEmbeddedCue());
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_cuesheet_test.flac";
    cdr::TrackMetadata meta;
    meta.album = "Album";
    writer.open(path, meta);
    writer.write(music.pcm.data(), music.pcm.size());
    writer.close();
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);

    const DecodedFlac d = decodeFlac(file);
    CHECK(d.pcm == music.pcm);
    CHECK(d.blockTypes == std::vector<int>({0, 4, 5, 3, 1}));  // CUESHEET after the tags
    // The CUESHEET tag holds the sheet without the .cue file's BOM.
    const std::string tag = "CUESHEET=FILE \"Image.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n";
    CHECK(std::find(d.comments.begin(), d.comments.end(), tag) != d.comments.end());
    CHECK(std::find(d.comments.begin(), d.comments.end(), "ALBUM=Album") != d.comments.end());

    const ParsedCueSheet c = parseCueSheet(flacBlock(file, 5));
    CHECK_EQ(c.tracks.size(), 4u);
    if (c.tracks.size() == 4) {
        CHECK_EQ(c.tracks[2].offset, uint64_t(75 * 588));
        CHECK_EQ(c.tracks[3].number, 170);
        CHECK_EQ(c.tracks[3].offset, uint64_t(99 * 588));
    }

    // Without a CUE sheet nothing changes.
    CHECK(cdr::WavWriter().canEmbedCueSheet() == false);
    const DecodedFlac plain = decodeFlac(encodeFlac(music.pcm, meta, 4096));
    CHECK(plain.blockTypes == std::vector<int>({0, 4, 3, 1}));
}

TEST(single_file_flac_rip_with_embedded_cuesheet) {
    // Whole disc into one FLAC with read offset correction: the CUESHEET
    // track positions point at each track's data, as in the per-track rips.
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::RipOptions options;
    options.readOffsetSamples = 667;
    cdr::Ripper ripper(drive, toc, options);
    cdr::AlbumMetadata album;
    album.title = "Album";
    const std::vector<cdr::CueTrack> tracks = cdr::singleFileCueTracks(toc.tracks, "Album.flac", album);
    cdr::EmbeddedCueSheet cue;
    cue.tracks = tracks;
    for (const cdr::Track& t : toc.tracks) cue.totalSectors += t.lengthSectors;
    cue.text = cdr::formatCueSheet(album, tracks);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "cdreader_image_test.flac";
    cdr::FlacWriter writer;
    writer.setEmbeddedCueSheet(cue);
    writer.open(path, album.forTrack(0, toc.lastTrack));
    std::vector<std::vector<uint8_t>> perTrack;
    for (const cdr::Track& t : toc.tracks) {
        perTrack.emplace_back();
        ripper.ripTrack(t, [&](const uint8_t* p, size_t n) {
            writer.write(p, n);
            perTrack.back().insert(perTrack.back().end(), p, p + n);
        });
    }
    writer.close();
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);

    const DecodedFlac d = decodeFlac(file);
    const ParsedCueSheet c = parseCueSheet(flacBlock(file, 5));
    CHECK_EQ(c.tracks.size(), toc.tracks.size() + 1);
    CHECK_EQ(c.tracks.back().offset, d.totalSamples);
    for (size_t i = 0; i < toc.tracks.size() && i < c.tracks.size(); ++i) {
        const size_t start = size_t(c.tracks[i].offset) * cdr::kBytesPerSample;
        CHECK_EQ(c.tracks[i].number, toc.tracks[i].number);
        CHECK(start + perTrack[i].size() <= d.pcm.size());
        if (start + perTrack[i].size() <= d.pcm.size())
            CHECK(std::equal(perTrack[i].begin(), perTrack[i].end(), d.pcm.begin() + long(start)));
    }
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
