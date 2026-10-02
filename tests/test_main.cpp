// Minimal self-contained test runner (no external dependencies).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/alac_encoder.h"
#include "cdreader/alac_writer.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/crc32.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/file_naming.h"
#include "cdreader/flac_encoder.h"
#include "cdreader/flac_writer.h"
#include "cdreader/gaps.h"
#include "cdreader/http.h"
#include "cdreader/md5.h"
#include "cdreader/metadata.h"
#include "cdreader/mp4.h"
#include "cdreader/ogg.h"
#include "cdreader/ogg_flac_writer.h"
#include "cdreader/resampler.h"
#include "cdreader/ripper.h"
#include "cdreader/scsi.h"
#include "cdreader/subchannel.h"
#include "cdreader/tags.h"
#include "cdreader/toc.h"
#include "cdreader/wav_writer.h"
#ifdef CDREADER_HAVE_OPUS
#include <opus.h>

#include "cdreader/opus_writer.h"
#endif
#ifdef CDREADER_HAVE_VORBIS
#include <vorbis/codec.h>

#include "cdreader/vorbis_writer.h"
#endif
#include "alac_decoder.h"
#include "fake_drive.h"
#include "flac_decoder.h"
#include "test_signals.h"
#include "test_temp.h"

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
        const std::string extension = f == "vorbis" ? "ogg" : f == "oggflac" ? "oga" : f == "alac" ? "m4a" : f.rfind("mka", 0) == 0 ? "mka" : f;
        CHECK(w != nullptr && w->extension() == extension);
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
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.wav";
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
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.flac";
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
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_partial.flac";
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
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test_tags.wav";
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

        const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test_image.wav";
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
    std::string mcn;  // up to the first NUL
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
        std::string isrc;
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
    c.mcn = std::string(reinterpret_cast<const char*>(b.data()), 128).c_str();
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
        e.isrc = std::string(reinterpret_cast<const char*>(b.data() + pos + 9), 12).c_str();
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
    auto track = [](int number, uint32_t start, bool preEmphasis, const char* title) {
        cdr::CueTrack t;
        t.number = number;
        t.file = "Image.flac";
        t.startSectors = start;
        t.preEmphasis = preEmphasis;
        t.copyPermitted = preEmphasis;
        t.title = title;
        return t;
    };
    cue.tracks = {track(1, 0, false, "One"), track(2, 30, true, "Two"), track(3, 75, false, "Three")};
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
    CHECK_EQ(be64At(block, cdr::flac::cueSheetLeadOutOffsetPosition(cue)), uint64_t(100 * 588));

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
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_cuesheet_test.flac";
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

    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_image_test.flac";
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

// --- Ogg FLAC (FLAC-to-Ogg mapping 1.0) -----------------------------------------

namespace {

// Number of packets that end on each page.
size_t packetsEndingOn(const OggPage& p) {
    size_t n = 0;
    for (uint8_t l : p.lacing) n += l < 255 ? 1 : 0;
    return n;
}

struct OggFlac {
    std::vector<OggPage> pages;
    std::vector<std::vector<uint8_t>> packets;
    size_t headerPackets = 0;     // packets before the audio, the first one included
    std::vector<uint8_t> native;  // the same stream as a native FLAC file, for decodeFlac()
};

// Checks the Ogg FLAC structure of `file` (header packets, pages, granule
// positions, BOS / EOS) and rebuilds the native FLAC stream from its packets.
OggFlac demuxOggFlac(const std::vector<uint8_t>& file) {
    OggFlac o;
    o.pages = parseOgg(file);
    o.packets = oggPackets(o.pages);
    CHECK(!o.pages.empty() && o.packets.size() >= 2);
    if (o.pages.empty() || o.packets.size() < 2) return o;

    // First packet: 0x7F "FLAC" 1.0, header packet count, "fLaC", STREAMINFO;
    // alone on the BOS page, granule position 0.
    const std::vector<uint8_t>& first = o.packets[0];
    CHECK_EQ(first.size(), cdr::oggflac::kFirstPacketBytes);
    CHECK(first.size() == 51 && first[0] == 0x7F && str(first, 1, 4) == "FLAC" && first[5] == 1 && first[6] == 0);
    CHECK(str(first, 9, 4) == "fLaC");
    CHECK_EQ(first[13] & 0x7F, 0);  // STREAMINFO
    CHECK(first[14] == 0 && first[15] == 0 && first[16] == 34);
    o.headerPackets = 1 + (size_t(first[7]) << 8 | first[8]);
    CHECK(o.headerPackets <= o.packets.size());
    if (o.headerPackets > o.packets.size()) return o;
    CHECK(o.pages[0].lacing == std::vector<uint8_t>({51}));
    CHECK_EQ(o.pages[0].granule, 0);

    // The header packets hold one metadata block each; only the last is flagged last.
    o.native = {'f', 'L', 'a', 'C'};
    o.native.insert(o.native.end(), first.begin() + 13, first.end());
    for (size_t i = 0; i < o.headerPackets; ++i) {
        const std::vector<uint8_t>& p = o.packets[i];
        const size_t block = i == 0 ? 13 : 0;
        CHECK(p.size() >= block + 4);
        if (p.size() < block + 4) return o;
        CHECK_EQ(p.size() - block - 4, size_t(p[block + 1]) << 16 | size_t(p[block + 2]) << 8 | p[block + 3]);
        CHECK_EQ(bool(p[block] & 0x80), i + 1 == o.headerPackets);
        if (i > 0) o.native.insert(o.native.end(), p.begin(), p.end());
    }
    for (size_t i = o.headerPackets; i < o.packets.size(); ++i)
        o.native.insert(o.native.end(), o.packets[i].begin(), o.packets[i].end());

    // Pages: the header pages (granule 0) end with the last header packet, the
    // audio starts on a new page; BOS first, EOS last; serial and sequence.
    size_t completed = 0;
    bool headerEnd = false;
    for (size_t i = 0; i < o.pages.size(); ++i) {
        const OggPage& p = o.pages[i];
        CHECK_EQ(p.serial, o.pages[0].serial);
        CHECK_EQ(p.sequence, uint32_t(i));
        CHECK_EQ(bool(p.flags & 0x02), i == 0);
        CHECK_EQ(bool(p.flags & 0x04), i + 1 == o.pages.size());
        if (completed < o.headerPackets) {
            CHECK_EQ(p.granule, 0);
            completed += packetsEndingOn(p);
            if (completed >= o.headerPackets) {
                CHECK_EQ(completed, o.headerPackets);
                CHECK_EQ(p.lacing.back() < 255, true);
                headerEnd = true;
            }
        } else {
            completed += packetsEndingOn(p);
        }
    }
    CHECK(headerEnd);
    CHECK_EQ(completed, o.packets.size());
    return o;
}

// Granule positions of the audio pages: the samples of the frames completed
// so far (-1 on pages where no frame ends).
void checkOggFlacGranules(const OggFlac& o, uint64_t totalSamples) {
    const size_t frames = o.packets.size() - o.headerPackets;
    auto samplesThrough = [&](size_t framesDone) {
        return std::min<uint64_t>(uint64_t(framesDone) * cdr::flac::kBlockSize, totalSamples);
    };
    size_t completed = 0;
    for (const OggPage& p : o.pages) {
        const size_t ending = packetsEndingOn(p);
        const bool header = completed < o.headerPackets;
        completed += ending;
        if (header) continue;
        if (ending == 0) {
            CHECK_EQ(p.granule, -1);
        } else {
            CHECK_EQ(uint64_t(p.granule), samplesThrough(completed - o.headerPackets));
        }
    }
    CHECK_EQ(completed - o.headerPackets, frames);
    if (frames > 0) CHECK_EQ(uint64_t(o.pages.back().granule), totalSamples);
}

std::vector<uint8_t> encodeOggFlac(const std::vector<uint8_t>& pcm, const cdr::TrackMetadata& meta,
                                   const cdr::EmbeddedCueSheet* cue = nullptr, size_t chunk = cdr::kSectorBytes) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.oga";
    {
        cdr::OggFlacWriter writer;
        CHECK(writer.canEmbedCueSheet());
        if (cue) writer.setEmbeddedCueSheet(*cue);
        writer.open(path, meta);
        size_t pos = 0;
        if (pcm.size() > 7) {  // an odd split to exercise partial samples
            writer.write(pcm.data(), 7);
            pos = 7;
        }
        for (; pos < pcm.size(); pos += chunk) writer.write(pcm.data() + pos, std::min(chunk, pcm.size() - pos));
        writer.close();
        CHECK_EQ(writer.totalSamples(), uint64_t(pcm.size() / 4));
    }
    std::vector<uint8_t> file = readFile(path);
    std::filesystem::remove(path);
    return file;
}

}  // namespace

TEST(oggflac_first_packet_layout) {
    std::vector<uint8_t> info(34);
    for (size_t i = 0; i < info.size(); ++i) info[i] = uint8_t(0xA0 + i);
    const std::vector<uint8_t> p = cdr::oggflac::firstPacket(2, info);
    std::vector<uint8_t> expected = {0x7F, 'F', 'L', 'A', 'C', 0x01, 0x00, 0x00, 0x02,
                                     'f',  'L', 'a', 'C', 0x00, 0x00, 0x00, 0x22};
    expected.insert(expected.end(), info.begin(), info.end());
    CHECK(p == expected);
    CHECK_EQ(p.size(), cdr::oggflac::kFirstPacketBytes);
    CHECK_EQ(cdr::oggflac::firstPacket(0, info)[13], 0x80);  // no other block: STREAMINFO is the last
    CHECK_EQ(cdr::oggflac::firstPacket(258, info)[7], 1);
    CHECK_EQ(cdr::oggflac::firstPacket(258, info)[8], 2);
    bool threw = false;
    try {
        cdr::oggflac::firstPacket(1, std::vector<uint8_t>(33));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(oggflac_writer_stream_structure_and_pcm) {
    cdr::TrackMetadata meta = sampleAlbum().forTrack(2, 3);
    for (size_t samples : {size_t(0), size_t(1), size_t(4096), size_t(4097), size_t(4096 * 25 + 1000)}) {
        const testsig::Signal music = testsig::music(samples);
        const std::vector<uint8_t> file = encodeOggFlac(music.pcm, meta, nullptr, 1000);
        if (samples == 4096 * 25 + 1000) keepSample("music.oga", file);
        const OggFlac o = demuxOggFlac(file);
        CHECK_EQ(o.headerPackets, 2u);  // STREAMINFO, VORBIS_COMMENT
        if (o.native.empty()) continue;
        const DecodedFlac d = decodeFlac(o.native);
        CHECK(d.pcm == music.pcm);
        CHECK(d.md5 == md5Of(music.pcm));
        CHECK_EQ(d.totalSamples, uint64_t(samples));
        CHECK_EQ(d.sampleRate, 44100u);
        CHECK_EQ(d.channels, 2u);
        CHECK_EQ(d.bitsPerSample, 16u);
        const unsigned block = samples > 4096 ? 4096u : unsigned(std::max<size_t>(samples, 16));
        CHECK(d.minBlockSize == block && d.maxBlockSize == block);
        CHECK(d.blockTypes == std::vector<int>({0, 4}));  // no SEEKTABLE / PADDING in Ogg
        CHECK(d.vendor.rfind("cdreader ", 0) == 0);
        CHECK(std::find(d.comments.begin(), d.comments.end(), "TITLE=Two \"2\"") != d.comments.end());
        CHECK(std::find(d.comments.begin(), d.comments.end(), "ARTIST=Guest") != d.comments.end());

        // One frame per audio packet, the frame size bounds of STREAMINFO match.
        const size_t frames = o.packets.size() - o.headerPackets;
        CHECK_EQ(size_t(d.frames), frames);
        CHECK_EQ(frames, (samples + 4095) / 4096);
        uint64_t offset = 0;
        uint32_t minFrame = UINT32_MAX, maxFrame = 0;
        for (size_t i = 0; i < frames && i < d.frameOffsets.size(); ++i) {
            const std::vector<uint8_t>& packet = o.packets[o.headerPackets + i];
            CHECK_EQ(d.frameOffsets[i], offset);
            offset += packet.size();
            minFrame = std::min(minFrame, uint32_t(packet.size()));
            maxFrame = std::max(maxFrame, uint32_t(packet.size()));
        }
        CHECK_EQ(d.minFrameSize, frames ? minFrame : 0u);
        CHECK_EQ(d.maxFrameSize, maxFrame);
        checkOggFlacGranules(o, samples);
        if (samples == 0) CHECK_EQ(o.pages.size(), 2u);  // the header pages, the last one with EOS

        // The frames are exactly those of the native FLAC writer.
        const std::vector<uint8_t> flac = encodeFlac(music.pcm, meta);
        const DecodedFlac nd = decodeFlac(flac);
        CHECK(std::equal(o.native.begin() + ptrdiff_t(d.firstFrame), o.native.end(),
                         flac.begin() + ptrdiff_t(nd.firstFrame), flac.end()));
        CHECK(d.md5 == nd.md5 && d.minFrameSize == nd.minFrameSize && d.maxFrameSize == nd.maxFrameSize);
    }
}

TEST(oggflac_writer_round_trip_synthetic_signals) {
    for (const testsig::Signal& s : testsig::all()) {
        const OggFlac o = demuxOggFlac(encodeOggFlac(s.pcm, {}));
        if (o.native.empty()) continue;
        const DecodedFlac d = decodeFlac(o.native);
        const bool ok = d.pcm == s.pcm && d.md5 == md5Of(s.pcm) && d.totalSamples == s.pcm.size() / 4;
        if (!ok) std::fprintf(stderr, "  Ogg FLAC round trip failed: %s\n", s.name.c_str());
        CHECK(ok);
        checkOggFlacGranules(o, s.pcm.size() / 4);
    }
}

TEST(oggflac_writer_embeds_cuesheet) {
    // As with native FLAC: 100 sectors announced, 99 written; the lead-out follows the real length.
    const testsig::Signal music = testsig::music(99 * 588);
    const cdr::EmbeddedCueSheet cue = sampleEmbeddedCue();
    cdr::TrackMetadata meta;
    meta.album = "Album";
    const std::vector<uint8_t> file = encodeOggFlac(music.pcm, meta, &cue);
    keepSample("image.oga", file);
    const OggFlac o = demuxOggFlac(file);
    CHECK_EQ(o.headerPackets, 3u);  // STREAMINFO, VORBIS_COMMENT, CUESHEET
    if (o.native.empty()) return;
    const DecodedFlac d = decodeFlac(o.native);
    CHECK(d.pcm == music.pcm);
    CHECK(d.blockTypes == std::vector<int>({0, 4, 5}));
    const std::string tag = "CUESHEET=FILE \"Image.flac\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n";
    CHECK(std::find(d.comments.begin(), d.comments.end(), tag) != d.comments.end());
    const ParsedCueSheet c = parseCueSheet(flacBlock(o.native, 5));
    CHECK_EQ(c.tracks.size(), 4u);
    if (c.tracks.size() == 4) {
        CHECK_EQ(c.tracks[1].offset, uint64_t(30 * 588));
        CHECK_EQ(c.tracks[2].offset, uint64_t(75 * 588));
        CHECK_EQ(c.tracks[3].number, 170);
        CHECK_EQ(c.tracks[3].offset, uint64_t(99 * 588));
    }
    // The CUESHEET block is the same as in the native FLAC file.
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_cuesheet_test.flac";
    {
        cdr::FlacWriter flac;
        flac.setEmbeddedCueSheet(cue);
        flac.open(path, meta);
        flac.write(music.pcm.data(), music.pcm.size());
        flac.close();
    }
    CHECK(flacBlock(readFile(path), 5) == flacBlock(o.native, 5));
    std::filesystem::remove(path);
}

TEST(oggflac_writer_rejects_partial_sample) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_partial.oga";
    bool threw = false;
    {
        cdr::OggFlacWriter writer;
        writer.open(path, {});
        const uint8_t pcm[6] = {1, 2, 3, 4, 5, 6};
        writer.write(pcm, sizeof pcm);
        try {
            writer.close();
        } catch (const std::runtime_error&) {
            threw = true;
        }
    }
    std::filesystem::remove(path);
    CHECK(threw);
}

namespace {

// --- Resampler (44.1 -> 48 kHz for Opus) --------------------------------------

std::vector<float> resampleAll(const std::vector<float>& in, size_t chunk) {
    cdr::Resampler r(44100, 48000, 2);
    std::vector<float> out;
    for (size_t pos = 0; pos < in.size() / 2; pos += chunk)
        r.process(in.data() + pos * 2, std::min(chunk, in.size() / 2 - pos), out);
    r.finish(out);
    return out;
}

// Signal-to-error ratio in dB of `actual` against `reference` (same length).
double snrDb(const std::vector<double>& reference, const std::vector<double>& actual) {
    double signal = 0, error = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        signal += reference[i] * reference[i];
        error += (actual[i] - reference[i]) * (actual[i] - reference[i]);
    }
    return error == 0 ? 999.0 : 10 * std::log10(signal / error);
}

TEST(resampler_output_length_and_chunking) {
    cdr::Resampler r(44100, 48000, 2);
    CHECK_EQ(r.outputLength(0), 0u);
    CHECK_EQ(r.outputLength(147), 160u);
    CHECK_EQ(r.outputLength(1), 2u);             // 1.088, rounded up
    CHECK_EQ(r.outputLength(1000), 1089u);       // 1088.4 (opusenc: 1089 as well)
    CHECK_EQ(r.outputLength(44100), 48000u);
    CHECK_EQ(r.outputLength(588), 640u);          // one CD sector
    testsig::Noise noise(11);
    for (size_t frames : {size_t(0), size_t(1), size_t(2), size_t(146), size_t(147), size_t(148), size_t(1000),
                          size_t(44100 + 7)}) {
        std::vector<float> in(frames * 2);
        for (float& v : in) v = float(noise.next() * 0.5);
        const std::vector<float> whole = resampleAll(in, frames ? frames : 1);
        CHECK_EQ(whole.size(), size_t(2 * ((frames * 160 + 146) / 147)));
        CHECK_EQ(whole.size() / 2, size_t(r.outputLength(frames)));
        // Any split of the input gives the same output.
        CHECK(resampleAll(in, 588) == whole);
        CHECK(resampleAll(in, 1) == whole);
        CHECK(resampleAll(in, 4097) == whole);
    }
}

TEST(resampler_sine_accuracy) {
    // Sines across the band against the exact 48 kHz sines: the error contains
    // passband ripple, images / aliases and arithmetic noise.
    for (double freq : {100.0, 1000.0, 15000.0, 19500.0}) {
        const size_t n = 44100;
        std::vector<float> in(n * 2);
        for (size_t i = 0; i < n; ++i) {
            in[2 * i] = float(0.5 * std::sin(testsig::kTwoPi * freq * double(i) / 44100));
            in[2 * i + 1] = float(0.5 * std::cos(testsig::kTwoPi * freq * double(i) / 44100));
        }
        const std::vector<float> out = resampleAll(in, 4096);
        CHECK_EQ(out.size(), size_t(2 * 48000));
        std::vector<double> ref, got;
        for (size_t i = 500; i + 500 < 48000; ++i) {  // away from the start / end transients
            ref.push_back(0.5 * std::sin(testsig::kTwoPi * freq * double(i) / 48000));
            got.push_back(out[2 * i]);
            ref.push_back(0.5 * std::cos(testsig::kTwoPi * freq * double(i) / 48000));
            got.push_back(out[2 * i + 1]);
        }
        const double snr = snrDb(ref, got);
        std::printf("  resampler %5.0f Hz: SNR %.1f dB\n", freq, snr);
        CHECK(snr > 95);
    }
}

TEST(resampler_rejects_out_of_band_and_keeps_dc) {
    // DC passes with unity gain.
    std::vector<float> dc(20000 * 2, 0.25f);
    const std::vector<float> out = resampleAll(dc, 1000);
    for (size_t i = 400; i + 400 < out.size() / 2; ++i) CHECK(std::abs(out[2 * i] - 0.25f) < 1e-5f);
    // A tone in the transition band (20 .. 22.05 kHz) is attenuated; its image
    // at 44.1 - 21.9 = 22.2 kHz is in the stop band and must be gone.
    const size_t n = 44100;
    std::vector<float> high(n * 2);
    for (size_t i = 0; i < n; ++i)
        high[2 * i] = high[2 * i + 1] = float(0.9 * std::sin(testsig::kTwoPi * 21900.0 * double(i) / 44100));
    const std::vector<float> filtered = resampleAll(high, 4096);
    // Amplitude of the component at `freq` (Hann windowed DFT, scaled to the sine amplitude).
    auto amplitude = [&](double freq) {
        double re = 0, im = 0, wsum = 0;
        const size_t a = 1000, b = filtered.size() / 2 - 1000;
        for (size_t i = a; i < b; ++i) {
            const double w = 0.5 - 0.5 * std::cos(testsig::kTwoPi * double(i - a) / double(b - a));
            re += w * filtered[2 * i] * std::cos(testsig::kTwoPi * freq * double(i) / 48000);
            im += w * filtered[2 * i] * std::sin(testsig::kTwoPi * freq * double(i) / 48000);
            wsum += w;
        }
        return 2 * std::sqrt(re * re + im * im) / wsum;
    };
    const double tone = amplitude(21900), image = amplitude(22200);
    std::printf("  resampler 21.9 kHz tone: passed %.1f dB, image at 22.2 kHz %.1f dB\n",
                20 * std::log10(tone / 0.9), 20 * std::log10(image / 0.9 + 1e-30));
    CHECK(tone < 0.9 * 0.01);    // below -40 dB (transition band)
    CHECK(image < 0.9 * 1e-5);   // below -100 dB (stop band)
}

#if defined(CDREADER_HAVE_OPUS) || defined(CDREADER_HAVE_VORBIS)

// --- Ogg helpers for the lossy writers -----------------------------------------

std::vector<uint8_t> encodeWith(cdr::AudioWriter& writer, const std::filesystem::path& path,
                                const std::vector<uint8_t>& pcm, const cdr::TrackMetadata& meta) {
    writer.open(path, meta);
    size_t pos = 0;
    if (pcm.size() > 7) {  // an odd split to exercise partial samples
        writer.write(pcm.data(), 7);
        pos = 7;
    }
    for (; pos < pcm.size(); pos += 2352 * 7) writer.write(pcm.data() + pos, std::min<size_t>(2352 * 7, pcm.size() - pos));
    writer.close();
    return readFile(path);
}

cdr::TrackMetadata lossyMetadata() {
    cdr::TrackMetadata m;
    m.trackNumber = 3;
    m.trackTotal = 12;
    m.title = "\xE6\x9B\xB2\xE5\x90\x8D";  // 曲名
    m.artist = "Artist";
    m.album = "Album";
    m.albumArtist = "Album Artist";
    m.year = "1999";
    m.genre = "Rock";
    m.discId = "0A0B0C03";
    return m;
}

bool containsText(const std::vector<uint8_t>& v, const std::string& text) {
    const std::vector<uint8_t> t(text.begin(), text.end());
    return std::search(v.begin(), v.end(), t.begin(), t.end()) != v.end();
}

#endif

TEST(audio_writer_settings_validation) {
    auto throws = [](const std::string& format, const cdr::EncoderSettings& s) {
        try {
            cdr::createAudioWriter(format, s);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    cdr::EncoderSettings bitrate;
    bitrate.bitrateKbps = 128;
    cdr::EncoderSettings quality;
    quality.quality = 4;
    CHECK(throws("wav", bitrate));
    CHECK(throws("flac", quality));
    CHECK(!throws("flac", {}));
    CHECK(throws("oggflac", bitrate));
    CHECK(throws("oggflac", quality));
    CHECK(!throws("oggflac", {}));
    CHECK(cdr::createAudioWriter("oggflac")->encoderDescription() == "Ogg FLAC (built-in encoder), lossless");
    CHECK(cdr::createAudioWriter("oggflac")->canEmbedCueSheet());
    CHECK(!cdr::isLossyFormat("flac") && !cdr::isLossyFormat("wav") && !cdr::isLossyFormat("oggflac"));
    CHECK(cdr::isLossyFormat("opus") && cdr::isLossyFormat("vorbis"));
#ifdef CDREADER_HAVE_OPUS
    CHECK(!throws("opus", bitrate));
    CHECK(throws("opus", quality));
    for (int kbps : {5, 511, 0, -1}) {
        cdr::EncoderSettings s;
        s.bitrateKbps = kbps;
        CHECK(throws("opus", s));
    }
    for (int kbps : {6, 510}) {
        cdr::EncoderSettings s;
        s.bitrateKbps = kbps;
        CHECK(!throws("opus", s));
    }
    CHECK(cdr::createAudioWriter("opus")->encoderDescription().find("VBR 160 kbit/s") != std::string::npos);
#endif
#ifdef CDREADER_HAVE_VORBIS
    CHECK(!throws("vorbis", bitrate));
    CHECK(!throws("vorbis", quality));
    cdr::EncoderSettings both = bitrate;
    both.quality = 3;
    CHECK(throws("vorbis", both));
    for (double q : {-1.5, 10.5}) {
        cdr::EncoderSettings s;
        s.quality = q;
        CHECK(throws("vorbis", s));
    }
    for (int kbps : {44, 501}) {
        cdr::EncoderSettings s;
        s.bitrateKbps = kbps;
        CHECK(throws("vorbis", s));
    }
    CHECK(cdr::createAudioWriter("vorbis")->encoderDescription().find("VBR quality 5") != std::string::npos);
    CHECK(cdr::createAudioWriter("vorbis", quality)->extension() == "ogg");
#endif
}

#ifdef CDREADER_HAVE_OPUS

TEST(opus_header_packets) {
    const std::vector<uint8_t> head = cdr::opus::headPacket(2, 312, 44100);
    const std::vector<uint8_t> expected = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01,
                                           0x44, 0xAC, 0x00, 0x00, 0x00, 0x00, 0};
    CHECK(head == expected);

    const cdr::TrackMetadata m = lossyMetadata();
    const std::vector<uint8_t> tags = cdr::opus::tagsPacket(m, "vendor");
    CHECK(str(tags, 0, 8) == "OpusTags");
    CHECK_EQ(le32At(tags, 8), 6u);
    CHECK(str(tags, 12, 6) == "vendor");
    CHECK_EQ(le32At(tags, 18), 9u);  // fields
    size_t pos = 22;
    std::vector<std::string> fields;
    for (int i = 0; i < 9; ++i) {
        const uint32_t len = le32At(tags, pos);
        fields.push_back(str(tags, pos + 4, len));
        pos += 4 + len;
    }
    CHECK_EQ(pos, tags.size());
    const std::vector<std::string> want = {"TITLE=" + m.title, "ARTIST=Artist", "ALBUM=Album",
                                           "ALBUMARTIST=Album Artist", "TRACKNUMBER=3", "TRACKTOTAL=12",
                                           "DATE=1999", "GENRE=Rock", "CDDB=0A0B0C03"};
    CHECK(fields == want);
    CHECK(cdr::opus::libraryVersion().rfind("libopus ", 0) == 0);
}

struct DecodedOpus {
    uint16_t preSkip = 0;
    int64_t finalGranule = 0;
    size_t packets = 0;
    std::vector<float> pcm;  // 48 kHz stereo, pre-skip removed, end trimmed
};

// Checks the Ogg Opus structure (RFC 7845) of `file` and decodes it with libopus.
DecodedOpus checkAndDecodeOpus(const std::vector<uint8_t>& file) {
    DecodedOpus d;
    const std::vector<OggPage> pages = parseOgg(file);
    const std::vector<std::vector<uint8_t>> packets = oggPackets(pages);
    CHECK(pages.size() >= 3);
    CHECK(packets.size() >= 3);
    if (pages.size() < 3 || packets.size() < 3) return d;
    // Headers: each alone on its page, granule 0.
    CHECK_EQ(pages[0].flags, 0x02);
    CHECK_EQ(packetsEndingOn(pages[0]), 1u);
    CHECK_EQ(pages[0].granule, 0);
    CHECK_EQ(pages[1].flags, 0x00);
    CHECK_EQ(packetsEndingOn(pages[1]), 1u);
    CHECK_EQ(pages[1].granule, 0);
    CHECK_EQ(packets[0].size(), 19u);
    CHECK(str(packets[0], 0, 8) == "OpusHead");
    d.preSkip = uint16_t(packets[0][10] | packets[0][11] << 8);
    CHECK(packets[0] == cdr::opus::headPacket(2, d.preSkip, 44100));
    CHECK(str(packets[1], 0, 8) == "OpusTags");
    // Audio pages: granule = 960 * packets so far, except the last page,
    // which ends the stream at the exact length (end trimming).
    size_t completed = 0;
    for (size_t i = 2; i < pages.size(); ++i) {
        const OggPage& p = pages[i];
        completed += packetsEndingOn(p);
        const bool last = i + 1 == pages.size();
        CHECK_EQ(bool(p.flags & 0x04), last);
        CHECK_EQ(p.flags & 0x02, 0);
        if (packetsEndingOn(p) == 0) {
            CHECK_EQ(p.granule, -1);
        } else if (!last) {
            CHECK_EQ(p.granule, int64_t(completed) * 960);
        }
    }
    d.packets = packets.size() - 2;
    CHECK_EQ(completed, d.packets);
    d.finalGranule = pages.back().granule;
    CHECK(d.finalGranule >= int64_t(d.preSkip));
    CHECK(d.finalGranule <= int64_t(d.packets) * 960);
    CHECK(d.finalGranule > int64_t(d.packets - 1) * 960);  // only the last packet is trimmed

    int error = 0;
    OpusDecoder* decoder = opus_decoder_create(48000, 2, &error);
    CHECK(error == OPUS_OK);
    std::vector<float> all;
    std::vector<float> frame(5760 * 2);
    for (size_t i = 2; i < packets.size(); ++i) {
        CHECK_EQ(opus_packet_get_nb_samples(packets[i].data(), opus_int32(packets[i].size()), 48000), 960);
        const int n = opus_decode_float(decoder, packets[i].data(), opus_int32(packets[i].size()), frame.data(), 5760, 0);
        CHECK_EQ(n, 960);
        if (n > 0) all.insert(all.end(), frame.begin(), frame.begin() + n * 2);
    }
    opus_decoder_destroy(decoder);
    const size_t begin = size_t(d.preSkip) * 2;
    const size_t end = size_t(d.finalGranule) * 2;
    if (end <= all.size() && begin <= end) d.pcm.assign(all.begin() + ptrdiff_t(begin), all.begin() + ptrdiff_t(end));
    return d;
}

TEST(opus_writer_stream_structure_and_length) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.opus";
    const cdr::Resampler lengths(44100, 48000, 2);
    for (size_t samples : {size_t(0), size_t(1), size_t(147), size_t(1000), size_t(44100), size_t(44100 * 3 + 17)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        cdr::OpusWriter writer;
        const std::vector<uint8_t> file = encodeWith(writer, path, s.pcm, lossyMetadata());
        if (samples == 44100 * 3 + 17) keepSample("tones.opus", file);
        const DecodedOpus d = checkAndDecodeOpus(file);
        CHECK_EQ(d.preSkip, writer.preSkip());
        CHECK(d.preSkip > 0);
        CHECK_EQ(d.finalGranule, int64_t(d.preSkip + lengths.outputLength(samples)));
        CHECK_EQ(uint64_t(d.finalGranule), writer.finalGranulePosition());
        CHECK_EQ(d.pcm.size(), size_t(2 * lengths.outputLength(samples)));
        CHECK(containsText(file, "TITLE=" + lossyMetadata().title));
    }
    std::filesystem::remove(path);
}

TEST(opus_writer_decodes_close_to_input) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.opus";
    const size_t samples = 44100 * 4 + 5;
    const testsig::Signal s = testsig::tonesPcm(samples);
    // The exact signal at 48 kHz (computed once, it is slow to evaluate).
    const size_t n48 = size_t(cdr::Resampler(44100, 48000, 2).outputLength(samples));
    std::vector<double> exact(2 * n48 + 4);
    for (size_t i = 0; i < n48 + 2; ++i) {
        exact[2 * i] = testsig::tones(double(i) / 48000, 0);
        exact[2 * i + 1] = testsig::tones(double(i) / 48000, 1);
    }
    std::vector<size_t> sizes;
    for (int kbps : {64, 160, 256}) {
        cdr::OpusWriter writer(kbps);
        const std::vector<uint8_t> file = encodeWith(writer, path, s.pcm, {});
        sizes.push_back(file.size());
        const DecodedOpus d = checkAndDecodeOpus(file);
        const size_t n = d.pcm.size() / 2;
        CHECK_EQ(n, n48);
        // Against the exact signal at 48 kHz, so that the resampler and the
        // pre-skip (alignment) are checked as well; lags around 0 must be worse.
        auto snrAtLag = [&](int lag) {
            std::vector<double> ref, got;
            for (size_t i = 2400; i + 2400 < n; ++i) {
                const size_t j = size_t(int64_t(i) + lag);
                ref.push_back(exact[2 * j]);
                got.push_back(d.pcm[2 * i]);
                ref.push_back(exact[2 * j + 1]);
                got.push_back(d.pcm[2 * i + 1]);
            }
            return snrDb(ref, got);
        };
        const double snr = snrAtLag(0);
        std::printf("  opus %3d kbit/s: %6zu bytes, SNR %.1f dB (lag -1: %.1f, +1: %.1f)\n", kbps, file.size(), snr,
                    snrAtLag(-1), snrAtLag(1));
        CHECK(snr > (kbps >= 160 ? 20 : 12));
        CHECK(snr > snrAtLag(-1) + 3 && snr > snrAtLag(1) + 3);
    }
    CHECK(sizes[0] < sizes[1] && sizes[1] < sizes[2]);
    std::filesystem::remove(path);
}

TEST(opus_writer_rejects_partial_sample) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_partial.opus";
    cdr::OpusWriter writer;
    writer.open(path, {});
    const uint8_t bytes[3] = {1, 2, 3};
    writer.write(bytes, 3);
    bool threw = false;
    try {
        writer.close();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(path);
}

#endif  // CDREADER_HAVE_OPUS

#ifdef CDREADER_HAVE_VORBIS

struct DecodedVorbis {
    int64_t finalGranule = 0;
    std::vector<uint8_t> comment;
    std::vector<float> pcm;  // 44.1 kHz stereo
};

// Checks the Ogg Vorbis page layout of `file` and decodes it with libvorbis.
DecodedVorbis checkAndDecodeVorbis(const std::vector<uint8_t>& file) {
    DecodedVorbis d;
    const std::vector<OggPage> pages = parseOgg(file);
    const std::vector<std::vector<uint8_t>> packets = oggPackets(pages);
    CHECK(pages.size() >= 3 && packets.size() >= 4);
    if (pages.size() < 3 || packets.size() < 4) return d;
    CHECK_EQ(pages[0].flags, 0x02);
    CHECK_EQ(packetsEndingOn(pages[0]), 1u);  // identification header alone
    CHECK_EQ(packetsEndingOn(pages[1]), 2u);  // comment + setup, then audio on a fresh page
    CHECK_EQ(pages[1].granule, 0);
    CHECK(packets[0][0] == 1 && str(packets[0], 1, 6) == "vorbis");
    CHECK_EQ(packets[0][11], 2);               // channels
    CHECK_EQ(le32At(packets[0], 12), 44100u);  // sample rate
    CHECK(packets[1][0] == 3 && str(packets[1], 1, 6) == "vorbis");
    CHECK(packets[2][0] == 5 && str(packets[2], 1, 6) == "vorbis");
    d.comment = packets[1];
    int64_t last = 0;
    for (size_t i = 2; i < pages.size(); ++i) {
        CHECK_EQ(bool(pages[i].flags & 0x04), i + 1 == pages.size());
        if (pages[i].granule != -1) {
            CHECK(pages[i].granule >= last);
            last = pages[i].granule;
        }
    }
    d.finalGranule = pages.back().granule;

    vorbis_info vi;
    vorbis_comment vc;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    for (size_t i = 0; i < packets.size(); ++i) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char*>(packets[i].data());
        op.bytes = long(packets[i].size());
        op.b_o_s = i == 0;
        op.e_o_s = i + 1 == packets.size();
        op.granulepos = i + 1 == packets.size() ? d.finalGranule : -1;
        op.packetno = ogg_int64_t(i);
        if (i < 3) {
            CHECK_EQ(vorbis_synthesis_headerin(&vi, &vc, &op), 0);
            if (i == 2) break;
        }
    }
    CHECK_EQ(vi.channels, 2);
    CHECK_EQ(vi.rate, 44100);
    vorbis_dsp_state vd;
    vorbis_block vb;
    CHECK_EQ(vorbis_synthesis_init(&vd, &vi), 0);
    vorbis_block_init(&vd, &vb);
    for (size_t i = 3; i < packets.size(); ++i) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char*>(packets[i].data());
        op.bytes = long(packets[i].size());
        op.e_o_s = i + 1 == packets.size();
        op.granulepos = i + 1 == packets.size() ? d.finalGranule : -1;
        op.packetno = ogg_int64_t(i);
        CHECK_EQ(vorbis_synthesis(&vb, &op), 0);
        vorbis_synthesis_blockin(&vd, &vb);
        float** pcm = nullptr;
        int n;
        while ((n = vorbis_synthesis_pcmout(&vd, &pcm)) > 0) {
            for (int k = 0; k < n; ++k) {
                d.pcm.push_back(pcm[0][k]);
                d.pcm.push_back(pcm[1][k]);
            }
            vorbis_synthesis_read(&vd, n);
        }
    }
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return d;
}

TEST(vorbis_writer_stream_structure_and_length) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.ogg";
    for (size_t samples : {size_t(0), size_t(1), size_t(1000), size_t(44100), size_t(44100 * 3 + 17)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        cdr::VorbisWriter writer;
        const std::vector<uint8_t> file = encodeWith(writer, path, s.pcm, lossyMetadata());
        if (samples == 44100 * 3 + 17) keepSample("tones.ogg", file);
        const DecodedVorbis d = checkAndDecodeVorbis(file);
        CHECK_EQ(d.finalGranule, int64_t(samples));
        CHECK_EQ(writer.samples(), uint64_t(samples));
        CHECK_EQ(d.pcm.size(), 2 * samples);
        // Our tags in the comment header, libvorbis' vendor string.
        CHECK(str(d.comment, 11, 18) == "Xiph.Org libVorbis");
        CHECK(containsText(d.comment, "TITLE=" + lossyMetadata().title));
        CHECK(containsText(d.comment, "TRACKNUMBER=3"));
        CHECK(containsText(d.comment, "CDDB=0A0B0C03"));
        CHECK_EQ(d.comment.back(), 1);  // framing bit
    }
    std::filesystem::remove(path);
}

TEST(vorbis_writer_decodes_close_to_input) {
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test.ogg";
    const size_t samples = 44100 * 4 + 5;
    const testsig::Signal s = testsig::tonesPcm(samples);
    std::vector<size_t> sizes;
    for (double q : {0.0, 5.0, 8.0}) {
        cdr::VorbisWriter writer(q);
        const std::vector<uint8_t> file = encodeWith(writer, path, s.pcm, {});
        sizes.push_back(file.size());
        const DecodedVorbis d = checkAndDecodeVorbis(file);
        CHECK_EQ(d.pcm.size(), 2 * samples);
        std::vector<double> ref, got;
        for (size_t i = 0; i < samples && 2 * i + 1 < d.pcm.size(); ++i) {
            for (int c = 0; c < 2; ++c) {
                const uint8_t* p = s.pcm.data() + 4 * i + 2 * size_t(c);
                ref.push_back(double(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768);
                got.push_back(d.pcm[2 * i + size_t(c)]);
            }
        }
        const double snr = snrDb(ref, got);
        std::printf("  vorbis q%.0f: %6zu bytes, SNR %.1f dB\n", q, file.size(), snr);
        CHECK(snr > (q >= 5 ? 20 : 12));
    }
    CHECK(sizes[0] < sizes[1] && sizes[1] < sizes[2]);
    cdr::EncoderSettings abr;
    abr.bitrateKbps = 96;
    std::unique_ptr<cdr::AudioWriter> w = cdr::createAudioWriter("vorbis", abr);
    const std::vector<uint8_t> file = encodeWith(*w, path, s.pcm, {});
    CHECK_EQ(checkAndDecodeVorbis(file).pcm.size(), 2 * samples);
    const double kbps = double(file.size()) * 8 / (double(samples) / 44100) / 1000;
    std::printf("  vorbis --bitrate 96: %.0f kbit/s\n", kbps);
    CHECK(kbps > 60 && kbps < 140);
    std::filesystem::remove(path);
}

#endif  // CDREADER_HAVE_VORBIS

// --- ALAC / M4A (#24) ----------------------------------------------------------

void splitPcm(const std::vector<uint8_t>& pcm, std::vector<int32_t>& l, std::vector<int32_t>& r) {
    const size_t n = pcm.size() / 4;
    l.resize(n);
    r.resize(n);
    for (size_t i = 0; i < n; ++i) {
        l[i] = int16_t(uint16_t(pcm[4 * i] | pcm[4 * i + 1] << 8));
        r[i] = int16_t(uint16_t(pcm[4 * i + 2] | pcm[4 * i + 3] << 8));
    }
}

cdr::TrackMetadata alacSampleMetadata() {
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

std::vector<uint8_t> writeM4a(const std::vector<uint8_t>& pcm, const cdr::TrackMetadata& meta,
                              cdr::alac::EncoderOptions options = {}, const std::string& name = "alac.m4a",
                              unsigned* escaped = nullptr) {
    const std::filesystem::path path = cdr_test::testTempDir() / ("cdreader_test_" + name);
    {
        cdr::AlacWriter w(options);
        w.open(path, meta);
        // CD sector sized chunks, after an odd split in the middle of a sample.
        size_t pos = std::min<size_t>(pcm.size(), 7);
        w.write(pcm.data(), pos);
        for (; pos < pcm.size(); pos += 2352) w.write(pcm.data() + pos, std::min<size_t>(2352, pcm.size() - pos));
        w.close();
        CHECK_EQ(w.totalSamples(), uint64_t(pcm.size() / 4));
        if (escaped) *escaped = w.escapedFrames();
    }
    std::vector<uint8_t> file = readFile(path);
    std::filesystem::remove(path);
    return file;
}

TEST(alac_magic_cookie_bytes) {
    cdr::alac::SpecificConfig c;
    c.maxFrameBytes = 0x00012345;
    c.avgBitRate = 0x000ABCDE;
    const std::array<uint8_t, 24> cookie = cdr::alac::magicCookie(c);
    const std::array<uint8_t, 24> expected = {
        0x00, 0x00, 0x10, 0x00,  // frameLength 4096
        0x00,                    // compatibleVersion
        0x10,                    // bitDepth 16
        0x28, 0x0A, 0x0E,        // pb 40, mb 10, kb 14
        0x02,                    // numChannels
        0x00, 0xFF,              // maxRun 255
        0x00, 0x01, 0x23, 0x45,  // maxFrameBytes
        0x00, 0x0A, 0xBC, 0xDE,  // avgBitRate
        0x00, 0x00, 0xAC, 0x44,  // sampleRate 44100
    };
    CHECK(cookie == expected);
}

TEST(alac_adaptive_golomb_codes) {
    // History starts at mb = 10: k = log2(10 / 512 + 3) = 1, modulus 1.
    cdr::flac::BitWriter bw;
    const int32_t zero[] = {0};
    CHECK_EQ(cdr::alac::adaptiveGolomb(zero, 1, 17, &bw), uint64_t(1));  // "0"
    bw.alignToByte();
    CHECK(bw.bytes() == std::vector<uint8_t>(1, 0x00));

    // 3 -> 6: six 1-bits and the 0-bit; -1 -> 1: "10" (history 10 + 40 * 6 = 250).
    cdr::flac::BitWriter b2;
    const int32_t small[] = {3, -1};
    CHECK_EQ(cdr::alac::adaptiveGolomb(small, 2, 17, &b2), uint64_t(7 + 2));
    b2.alignToByte();
    CHECK(b2.bytes() == (std::vector<uint8_t>{0xFD, 0x00}));

    // A large first value is escaped: nine 1-bits and the value in 17 bits.
    cdr::flac::BitWriter b3;
    const int32_t large[] = {-65536};
    CHECK_EQ(cdr::alac::adaptiveGolomb(large, 1, 17, &b3), uint64_t(9 + 17));
    b3.alignToByte();
    // 1 1111 1111 | 1 1111 1111 1111 1111 (131071) -> 26 bits, padded
    CHECK(b3.bytes() == (std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xC0}));

    // A long run of zeros costs a few bits after the history has decayed.
    std::vector<int32_t> zeros(4096, 0);
    CHECK(cdr::alac::adaptiveGolomb(zeros.data(), 4096, 17, nullptr) < 40);
}

TEST(alac_frames_round_trip_synthetic_signals) {
    int compressed = 0, escaped = 0;
    std::set<unsigned> mixResSeen, ordersSeen;
    for (const testsig::Signal& s : testsig::all()) {
        if (s.pcm.empty()) continue;
        std::vector<int32_t> l, r;
        splitPcm(s.pcm, l, r);
        cdr::alac::FrameEncoder enc;
        std::vector<uint8_t> decoded;
        bool ok = true;
        for (size_t pos = 0; pos < l.size(); pos += cdr::alac::kFrameLength) {
            const unsigned n = unsigned(std::min<size_t>(cdr::alac::kFrameLength, l.size() - pos));
            const std::vector<uint8_t> frame = enc.encode(l.data() + pos, r.data() + pos, n);
            const AlacFrameInfo info = decodeAlacFrame(frame.data(), frame.size(), 4096, 40, 10, 14, decoded);
            ok = ok && info.samples == n && info.partial == (n != 4096) && info.escaped == enc.lastFrame().escaped;
            // An escape frame is the bound: 23 header bits, 32 bits per sample, end tag.
            ok = ok && frame.size() <= (23 + (n != 4096 ? 32 : 0) + 32 * size_t(n) + 3 + 7) / 8;
            if (!info.escaped) {
                ok = ok && info.mixBits == 2 && info.mixRes == enc.lastFrame().mixRes && info.mixRes <= 4;
                ok = ok && info.orderU == enc.lastFrame().orderU && info.orderV == enc.lastFrame().orderV;
                ok = ok && info.denShiftU == 9 && info.denShiftV == 9 && info.pbFactorU == 4 && info.modeU == 0;
                mixResSeen.insert(info.mixRes);
                ordersSeen.insert(info.orderU);
                ordersSeen.insert(info.orderV);
                ++compressed;
            } else {
                ++escaped;
            }
        }
        ok = ok && decoded == s.pcm;
        if (!ok) std::fprintf(stderr, "  ALAC round trip failed: %s\n", s.name.c_str());
        CHECK(ok);
    }
    CHECK(compressed > 0);
    CHECK(escaped > 0);  // white noise
    // The search uses every stereo matrix and predictor order on these signals.
    CHECK(mixResSeen == (std::set<unsigned>{0, 1, 2, 3, 4}));
    CHECK(ordersSeen == (std::set<unsigned>{0, 4, 8, 16}));
}

TEST(alac_uncompressed_fallback) {
    // Full-scale white noise does not compress: escape frames of 16-bit samples.
    testsig::Noise n(11);
    const testsig::Signal noise = testsig::make("noise", 4096 + 100, [&](size_t, int16_t& l, int16_t& r) {
        l = testsig::clamp16(n.next() * 32768);
        r = testsig::clamp16(n.next() * 32768);
    });
    std::vector<int32_t> l, r;
    splitPcm(noise.pcm, l, r);
    cdr::alac::FrameEncoder enc;
    std::vector<uint8_t> frame = enc.encode(l.data(), r.data(), 4096);
    CHECK(enc.lastFrame().escaped);
    CHECK_EQ(frame.size(), size_t((23 + 4096 * 32 + 3 + 7) / 8));
    CHECK_EQ(frame[0], 0x20);  // channel pair (001), instance 0, 12 zero bits ...
    CHECK_EQ((frame[2] >> 1) & 0x0F, 0x01);  // ... no partial frame, no shift, escape flag
    // First sample right after the 23-bit header.
    const uint32_t first = uint32_t(frame[2] & 0x01) << 15 | uint32_t(frame[3]) << 7 | frame[4] >> 1;
    CHECK_EQ(int16_t(first), int16_t(l[0]));
    std::vector<uint8_t> decoded;
    decodeAlacFrame(frame.data(), frame.size(), 4096, 40, 10, 14, decoded);
    frame = enc.encode(l.data() + 4096, r.data() + 4096, 100);  // partial: 32-bit length
    CHECK(enc.lastFrame().escaped);
    CHECK_EQ(frame.size(), size_t((23 + 32 + 100 * 32 + 3 + 7) / 8));
    CHECK_EQ(decodeAlacFrame(frame.data(), frame.size(), 4096, 40, 10, 14, decoded).samples, 100u);
    CHECK(decoded == noise.pcm);

    // Compression switched off: every frame escaped, still exact.
    cdr::alac::EncoderOptions off;
    off.compress = false;
    const testsig::Signal music = testsig::music(10000);
    unsigned escapedFrames = 0;
    const DecodedM4a d = decodeM4a(writeM4a(music.pcm, {}, off, "alac_raw.m4a", &escapedFrames));
    CHECK_EQ(escapedFrames, 3u);
    CHECK(d.pcm == music.pcm);
    for (const AlacFrameInfo& f : d.frames) CHECK(f.escaped);
}

TEST(alac_predictor_extreme_coefficients) {
    // Hand-built frames with extreme starting coefficients (wrapping int16
    // adaptation, large prediction sums): the decoder must reproduce the
    // samples whenever the encoder does not flag the frame as ambiguous.
    testsig::Noise noise(21);
    std::vector<int32_t> l(4096), r(4096);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = testsig::clamp16(30000 * std::sin(testsig::kTwoPi * 50 * double(i) / 44100) + noise.next() * 2000);
        r[i] = i % 64 < 32 ? 32767 : -32768;
    }
    int checked = 0;
    for (int16_t start : {int16_t(32767), int16_t(-32768), int16_t(12345), int16_t(-300)}) {
        for (unsigned order : {1u, 4u, 8u, 16u, 30u}) {
            std::vector<int16_t> cu(order, start), cv(order, int16_t(-start));
            std::vector<int16_t> au = cu, av = cv;
            std::vector<int32_t> ru(4096), rv(4096);
            bool ok = cdr::alac::predict(l.data(), 4096, au.data(), order, 17, ru.data());
            ok = cdr::alac::predict(r.data(), 4096, av.data(), order, 17, rv.data()) && ok;
            cdr::flac::BitWriter bw;
            bw.writeBits(1, 3);
            bw.writeBits(0, 4 + 12 + 4);
            bw.writeBits(2, 8);  // mixBits
            bw.writeBits(0, 8);  // mixRes: L / R
            for (const std::vector<int16_t>* c : {&cu, &cv}) {
                bw.writeBits(9, 8);
                bw.writeBits(4 << 5 | order, 8);
                for (int16_t x : *c) bw.writeSigned(x, 16);
            }
            bool ambiguous = false;
            cdr::alac::adaptiveGolomb(ru.data(), 4096, 17, &bw, &ambiguous);
            cdr::alac::adaptiveGolomb(rv.data(), 4096, 17, &bw, &ambiguous);
            bw.writeBits(7, 3);
            bw.alignToByte();
            if (!ok || ambiguous) continue;
            ++checked;
            std::vector<uint8_t> decoded;
            decodeAlacFrame(bw.bytes().data(), bw.bytes().size(), 4096, 40, 10, 14, decoded);
            std::vector<int32_t> dl, dr;
            splitPcm(decoded, dl, dr);
            CHECK(dl == l && dr == r);
        }
    }
    CHECK(checked > 10);
}

TEST(alac_m4a_box_structure_and_tables) {
    const testsig::Signal s = testsig::music(44100 * 2 + 1000);  // 22 frames, the last one partial
    const std::vector<uint8_t> file = writeM4a(s.pcm, alacSampleMetadata());
    keepSample("alac.m4a", file);
    const DecodedM4a d = decodeM4a(file);

    std::vector<std::string> top;
    for (const Mp4Box& b : d.root.children) top.push_back(b.type);
    CHECK(top == (std::vector<std::string>{"ftyp", "wide", "mdat", "moov"}));
    CHECK(d.majorBrand == "M4A ");
    CHECK(d.compatibleBrands == (std::vector<std::string>{"M4A ", "mp42", "isom"}));
    for (const char* path : {"moov/mvhd", "moov/trak/tkhd", "moov/trak/mdia/mdhd", "moov/trak/mdia/hdlr",
                             "moov/trak/mdia/minf/smhd", "moov/trak/mdia/minf/dinf/dref/url ",
                             "moov/trak/mdia/minf/stbl/stsd/alac/alac", "moov/trak/mdia/minf/stbl/stts",
                             "moov/trak/mdia/minf/stbl/stsc", "moov/trak/mdia/minf/stbl/stsz",
                             "moov/trak/mdia/minf/stbl/stco", "moov/udta/meta/hdlr", "moov/udta/meta/ilst"})
        CHECK(d.root.find(path) != nullptr);

    const uint64_t samples = s.pcm.size() / 4;
    CHECK_EQ(d.movieTimescale, 44100u);
    CHECK_EQ(d.mediaTimescale, 44100u);
    CHECK_EQ(d.movieDuration, samples);
    CHECK_EQ(d.trackDuration, samples);
    CHECK_EQ(d.mediaDuration, samples);
    CHECK(d.handlerType == "soun");
    CHECK(d.sampleEntryType == "alac");
    CHECK_EQ(d.entryChannels, 2u);
    CHECK_EQ(d.entrySampleSize, 16u);
    CHECK_EQ(d.entrySampleRate, 44100u);
    CHECK_EQ(d.frameLength, 4096u);
    CHECK_EQ(d.bitDepth, 16u);
    CHECK_EQ(d.channels, 2u);
    CHECK_EQ(d.sampleRate, 44100u);
    CHECK_EQ(d.compatibleVersion, 0u);
    CHECK_EQ(d.pb, 40u);
    CHECK_EQ(d.mb, 10u);
    CHECK_EQ(d.kb, 14u);
    CHECK_EQ(d.maxRun, 255u);

    // stts: 21 frames of 4096, one of the rest.
    const size_t frames = (samples + 4095) / 4096;
    CHECK_EQ(d.stts.size(), size_t(2));
    CHECK(d.stts[0] == std::make_pair(uint32_t(frames - 1), uint32_t(4096)));
    CHECK(d.stts[1] == std::make_pair(uint32_t(1), uint32_t(samples - (frames - 1) * 4096)));
    CHECK_EQ(d.sampleSizes.size(), frames);
    CHECK_EQ(d.stszSampleSize, 0u);  // sizes differ: listed per frame
    // stsc: 10 frames per chunk, then the remainder; stco points at each chunk.
    CHECK_EQ(d.stsc.size(), size_t(2));
    CHECK((d.stsc[0] == std::array<uint32_t, 3>{1, 10, 1}));
    CHECK((d.stsc[1] == std::array<uint32_t, 3>{3, uint32_t(frames - 20), 1}));
    CHECK_EQ(d.chunkOffsets.size(), size_t(3));
    CHECK(!d.co64);
    // Frames fill mdat back to back from its start.
    uint64_t offset = d.mdatDataOffset, total = 0;
    uint32_t largest = 0;
    for (size_t i = 0; i < frames; ++i) {
        CHECK_EQ(d.frameOffsets[i], offset);
        offset += d.sampleSizes[i];
        total += d.sampleSizes[i];
        largest = std::max(largest, d.sampleSizes[i]);
    }
    CHECK_EQ(total, d.mdatDataSize);
    CHECK_EQ(d.maxFrameBytes, largest);
    CHECK_EQ(d.avgBitRate, uint32_t(total * 8 * 44100 / samples));
    CHECK(total < s.pcm.size() / 2);  // the music signal compresses well

    CHECK(d.pcm == s.pcm);
    CHECK(d.metaHandler == "mdir");
    CHECK(d.tags.at("\xA9nam") == alacSampleMetadata().title);
    CHECK(d.tags.at("\xA9" "ART") == "Artist");
    CHECK(d.tags.at("\xA9" "alb") == "Album");
    CHECK(d.tags.at("aART") == "Album Artist");
    CHECK(d.tags.at("trkn") == "3/12");
    CHECK(d.tags.at("\xA9" "day") == "1999");
    CHECK(d.tags.at("\xA9gen") == "Rock");
    CHECK(d.tags.at("\xA9too").rfind("cdreader ", 0) == 0);
    CHECK(d.tags.at("----:com.apple.iTunes:CDDB") == "0A0B0C03");
    CHECK_EQ(d.tags.size(), size_t(9));
}

TEST(alac_m4a_isrc_and_barcode_tags) {
    cdr::TrackMetadata m = alacSampleMetadata();
    m.isrc = "JPXX01234567";
    m.mcn = "4988000000017";
    const DecodedM4a d = decodeM4a(writeM4a(std::vector<uint8_t>(4 * 4096, 0), m, {}, "alac_isrc.m4a"));
    CHECK(d.tags.at("----:com.apple.iTunes:ISRC") == "JPXX01234567");
    CHECK(d.tags.at("----:com.apple.iTunes:BARCODE") == "4988000000017");
    CHECK_EQ(d.tags.size(), size_t(11));
}

TEST(alac_m4a_edge_cases) {
    // No audio: valid boxes, no frames.
    const DecodedM4a empty = decodeM4a(writeM4a({}, {}, {}, "alac_empty.m4a"));
    CHECK_EQ(empty.movieDuration, uint64_t(0));
    CHECK(empty.stts.empty() && empty.sampleSizes.empty() && empty.chunkOffsets.empty());
    CHECK_EQ(empty.mdatDataSize, uint64_t(0));
    CHECK(empty.tags.count("\xA9too") == 1);
    CHECK_EQ(empty.tags.size(), size_t(1));  // only the encoder without metadata

    // Exactly one and exactly ten frames: a single stts / stsc entry.
    for (size_t n : {size_t(4096), size_t(40960)}) {
        const testsig::Signal s = testsig::music(n);
        const DecodedM4a d = decodeM4a(writeM4a(s.pcm, {}, {}, "alac_full.m4a"));
        CHECK_EQ(d.stts.size(), size_t(1));
        CHECK(d.stts[0] == std::make_pair(uint32_t(n / 4096), uint32_t(4096)));
        CHECK_EQ(d.stsc.size(), size_t(1));
        CHECK_EQ(d.chunkOffsets.size(), size_t(1));
        CHECK(d.pcm == s.pcm);
        for (const AlacFrameInfo& f : d.frames) CHECK(!f.partial);
    }

    // A single sample.
    const testsig::Signal one = testsig::make("one", 1, [](size_t, int16_t& l, int16_t& r) {
        l = -32768;
        r = 32767;
    });
    const DecodedM4a d1 = decodeM4a(writeM4a(one.pcm, {}, {}, "alac_one.m4a"));
    CHECK(d1.pcm == one.pcm);
    CHECK(d1.frames.size() == 1 && d1.frames[0].partial && d1.frames[0].samples == 1);
    CHECK_EQ(d1.stszSampleSize, d1.sampleSizes[0]);  // a constant size (FFmpeg needs it here)

    // Track number without total.
    cdr::TrackMetadata m;
    m.trackNumber = 7;
    CHECK(decodeM4a(writeM4a(one.pcm, m, {}, "alac_trk.m4a")).tags.at("trkn") == "7/0");

    // Input ending in the middle of a sample is an error.
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test_alac_partial.m4a";
    cdr::AlacWriter w;
    w.open(path, {});
    const uint8_t bytes[6] = {};
    w.write(bytes, sizeof bytes);
    bool threw = false;
    try {
        w.close();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove(path);
}

TEST(mp4_sample_table_chunks_and_64bit_offsets) {
    // Frames beyond 4 GB need co64; a gap in the file starts a new chunk.
    cdr::mp4::AudioTrack t;
    t.duration = 4096 * 3 + 5;
    t.sampleEntry = cdr::mp4::audioSampleEntry("alac", 2, 16, 44100, {});
    t.frameSizes = {100, 200, 300, 50};
    const uint64_t base = (uint64_t(1) << 32) + 16;
    t.frameOffsets = {base, base + 100, base + 1000, base + 1300};
    t.framesPerChunk = 10;
    const std::vector<uint8_t> moov = cdr::mp4::movie(t, {});
    const std::string text(moov.begin(), moov.end());
    CHECK(text.find("stco") == std::string::npos);
    const size_t co64 = text.find("co64");
    CHECK(co64 != std::string::npos);
    if (co64 != std::string::npos) {
        auto be = [&](size_t pos, int n) {
            uint64_t v = 0;
            for (int i = 0; i < n; ++i) v = v << 8 | moov[pos + size_t(i)];
            return v;
        };
        CHECK_EQ(be(co64 - 4, 4), uint64_t(8 + 4 + 4 + 2 * 8));  // two chunks
        CHECK_EQ(be(co64 + 8, 4), uint64_t(2));
        CHECK_EQ(be(co64 + 12, 8), base);
        CHECK_EQ(be(co64 + 20, 8), base + 1000);
        const size_t stsc = text.find("stsc");
        CHECK_EQ(be(stsc + 8, 4), uint64_t(1));  // one run: 2 frames per chunk
        CHECK_EQ(be(stsc + 12, 4), uint64_t(1));
        CHECK_EQ(be(stsc + 16, 4), uint64_t(2));
        const size_t stts = text.find("stts");
        CHECK_EQ(be(stts + 8, 4), uint64_t(2));
        CHECK_EQ(be(stts + 12, 4), uint64_t(3));
        CHECK_EQ(be(stts + 16, 4), uint64_t(4096));
        CHECK_EQ(be(stts + 20, 4), uint64_t(1));
        CHECK_EQ(be(stts + 24, 4), uint64_t(5));
    }
    // The duration must match the frames.
    t.duration = 4096 * 4 + 1;
    bool threw = false;
    try {
        cdr::mp4::movie(t, {});
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(alac_compression_against_flac) {
    // Our ALAC should be in the same league as our FLAC on music-like audio.
    const testsig::Signal s = testsig::music(44100 * 3);
    const DecodedM4a d = decodeM4a(writeM4a(s.pcm, {}, {}, "alac_ratio.m4a"));
    const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_test_ratio.flac";
    {
        cdr::FlacWriter w;
        w.open(path, {});
        w.write(s.pcm.data(), s.pcm.size());
        w.close();
    }
    const uintmax_t flacBytes = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    std::printf("  music 3 s: PCM %zu, ALAC frames %ju, FLAC file %ju bytes\n", s.pcm.size(),
                uintmax_t(d.mdatDataSize), flacBytes);
    CHECK(d.mdatDataSize < flacBytes * 13 / 10);
}

}  // namespace

// --- MCN / ISRC from the Q sub-channel (#22) ------------------------------------

namespace {

// A format 02h / 03h response as a drive returns it.
std::vector<uint8_t> subChannelResponse(uint8_t format, int track, bool valid, const std::string& code) {
    std::vector<uint8_t> r(24, 0);
    r[3] = 20;
    r[4] = format;
    if (format == 0x03) {
        r[5] = 0x30;
        r[6] = uint8_t(track);
    }
    r[8] = valid ? 0x80 : 0x00;
    std::memcpy(r.data() + 9, code.data(), std::min(code.size(), size_t(format == 0x02 ? 13 : 12)));
    return r;
}

using Status = cdr::SubChannelCode::Status;

}  // namespace

TEST(mcn_and_isrc_validation) {
    CHECK(cdr::isValidMcn("4988001234567"));
    CHECK(cdr::isValidMcn("0075678263927"));
    CHECK(!cdr::isValidMcn("0000000000000"));  // "no MCN" on some drives
    CHECK(!cdr::isValidMcn("498800123456"));   // 12 digits
    CHECK(!cdr::isValidMcn("498800123456A"));
    CHECK(!cdr::isValidMcn(std::string("498800123456\0", 13)));
    CHECK(cdr::isValidIsrc("JPVI09912345"));
    CHECK(cdr::isValidIsrc("USRC17607839"));
    CHECK(cdr::isValidIsrc("GBAYE0601498"));
    CHECK(!cdr::isValidIsrc("jpvi09912345"));   // lower case
    CHECK(!cdr::isValidIsrc("JP-VI0-99-12345"));
    CHECK(!cdr::isValidIsrc("J1VI09912345"));   // country: letters only
    CHECK(!cdr::isValidIsrc("JPV-09912345"));   // registrant: letters or digits
    CHECK(!cdr::isValidIsrc("JPVI0A912345"));   // year: digits
    CHECK(!cdr::isValidIsrc("JPVI099123X5"));   // designation: digits
    CHECK(!cdr::isValidIsrc("000000000000"));
    CHECK(!cdr::isValidIsrc(""));
}

TEST(subchannel_cdb_bytes) {
    FakeDrive fake = makeAudioDisc();
    cdr::CdDrive drive(fake);
    drive.readMcn();
    // READ SUB-CHANNEL, SubQ, format 02h, allocation length 24.
    CHECK(fake.lastSubChannelCdb == std::vector<uint8_t>({0x42, 0x00, 0x40, 0x02, 0, 0, 0, 0x00, 24, 0}));
    drive.readIsrc(3);
    CHECK(fake.lastSubChannelCdb == std::vector<uint8_t>({0x42, 0x00, 0x40, 0x03, 0, 0, 3, 0x00, 24, 0}));
    uint8_t buf[16];
    drive.readSubChannel(cdr::SubChannelFormat::CurrentPosition, 5, buf, sizeof buf, true);
    // MSF bit set, track ignored for the current position format.
    CHECK(fake.lastSubChannelCdb == std::vector<uint8_t>({0x42, 0x02, 0x40, 0x01, 0, 0, 0, 0x00, 16, 0}));
    CHECK_EQ(fake.subChannelCommands, 3);
    // Track numbers outside 1..99 never reach the drive.
    CHECK(drive.readIsrc(0).status == Status::Invalid);
    CHECK(drive.readIsrc(100).status == Status::Invalid);
    CHECK_EQ(fake.subChannelCommands, 3);
}

TEST(read_mcn_and_isrc_from_drive) {
    FakeDrive fake = makeAudioDisc();
    fake.mcn = "4988001234567";
    fake.isrcs[1] = "JPVI09912345";
    fake.isrcs[3] = "USRC17607839";
    cdr::CdDrive drive(fake);
    const cdr::SubChannelCode mcn = drive.readMcn();
    CHECK(mcn.found() && mcn.value == "4988001234567");
    CHECK(mcn.describe() == "4988001234567");
    CHECK(drive.readIsrc(1).value == "JPVI09912345");
    CHECK(drive.readIsrc(2).status == Status::NotPresent);  // TCVal = 0
    CHECK(drive.readIsrc(2).describe() == "not present");
    CHECK(drive.readIsrc(3).value == "USRC17607839");
}

TEST(subchannel_not_present_and_garbage) {
    // MCVal = 0, or MCVal = 1 with all zeros: no MCN.
    std::vector<uint8_t> r = subChannelResponse(0x02, 0, false, "4988001234567");
    CHECK(cdr::parseMcnResponse(r.data(), r.size()).status == Status::NotPresent);
    r = subChannelResponse(0x02, 0, true, "0000000000000");
    CHECK(cdr::parseMcnResponse(r.data(), r.size()).status == Status::NotPresent);
    r = subChannelResponse(0x02, 0, true, "4988001234567");
    CHECK(cdr::parseMcnResponse(r.data(), r.size()).value == "4988001234567");
    // Malformed codes with the valid bit set.
    r = subChannelResponse(0x02, 0, true, "49880012\x01" "4567");
    cdr::SubChannelCode c = cdr::parseMcnResponse(r.data(), r.size());
    CHECK(c.status == Status::Invalid && c.value.empty());
    CHECK(c.describe() == "invalid response (MCN \"49880012?4567\")");
    r = subChannelResponse(0x03, 2, true, "jp-vi0991234");
    CHECK(cdr::parseIsrcResponse(r.data(), r.size(), 2).status == Status::Invalid);
    r = subChannelResponse(0x03, 2, true, std::string(12, '\0'));
    CHECK(cdr::parseIsrcResponse(r.data(), r.size(), 2).status == Status::NotPresent);
    r = subChannelResponse(0x03, 2, false, "JPVI09912345");
    CHECK(cdr::parseIsrcResponse(r.data(), r.size(), 2).status == Status::NotPresent);
    // An answer for another track, or with the track left at 0.
    r = subChannelResponse(0x03, 3, true, "JPVI09912345");
    CHECK(cdr::parseIsrcResponse(r.data(), r.size(), 2).status == Status::Invalid);
    r = subChannelResponse(0x03, 0, true, "JPVI09912345");
    CHECK(cdr::parseIsrcResponse(r.data(), r.size(), 2).value == "JPVI09912345");
    // Wrong format code, too small data length, short transfer.
    r = subChannelResponse(0x03, 1, true, "JPVI09912345");
    CHECK(cdr::parseMcnResponse(r.data(), r.size()).status == Status::Invalid);
    r = subChannelResponse(0x02, 0, true, "4988001234567");
    r[3] = 12;
    CHECK(cdr::parseMcnResponse(r.data(), r.size()).status == Status::Invalid);
    r[3] = 20;
    CHECK(cdr::parseMcnResponse(r.data(), 23).status == Status::Invalid);
    CHECK(cdr::parseMcnResponse(r.data(), 0).status == Status::Invalid);
    CHECK(cdr::parseMcnResponse(nullptr, 24).status == Status::Invalid);

    // The same through the drive.
    FakeDrive fake = makeAudioDisc();
    fake.mcn = "12345";  // MCVal set, then 5 digits and NULs
    fake.isrcs[1] = "JPVI0991234";  // 11 characters
    cdr::CdDrive drive(fake);
    CHECK(drive.readMcn().status == Status::Invalid);
    CHECK(drive.readIsrc(1).status == Status::Invalid);
    fake.mcn = "4988001234567";
    fake.subChannelTransferLimit = 20;  // the drive returns fewer bytes than asked for
    c = drive.readMcn();
    CHECK(c.status == Status::Invalid);
    CHECK(c.detail == "short response (20 of 24 bytes)");
}

TEST(subchannel_unsupported_and_errors) {
    FakeDrive fake = makeAudioDisc();
    fake.mcn = "4988001234567";
    fake.subChannelSupported = false;  // ILLEGAL REQUEST, invalid command operation code
    cdr::CdDrive drive(fake);
    CHECK(drive.readMcn().status == Status::Unsupported);
    CHECK(drive.readIsrc(1).status == Status::Unsupported);
    CHECK(drive.readMcn().describe() == "not supported by the drive");

    fake.subChannelSupported = true;
    fake.discPresent = false;  // NOT READY: another failure
    const cdr::SubChannelCode c = drive.readMcn();
    CHECK(c.status == Status::Failed);
    CHECK(c.detail.find("NOT READY") != std::string::npos);

    cdr::ScsiResult transport;
    transport.error = "device gone";
    CHECK(cdr::subChannelError(transport).status == Status::Failed);
    CHECK(cdr::subChannelError(transport).describe() == "read failed (device gone)");
}

TEST(read_disc_codes_for_tracks) {
    // Track 2 is a data track: no ISRC read.
    FakeDrive fake({{0, false}, {300, true}, {450, false}}, 750);
    fake.mcn = "4988001234567";
    fake.isrcs[1] = "JPVI09912345";
    fake.isrcs[2] = "JPVI09900000";
    fake.isrcs[3] = "bad";
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscCodes codes = cdr::readDiscCodes(drive, toc.tracks);
    CHECK(codes.read);
    CHECK_EQ(fake.subChannelCommands, 3);
    CHECK(codes.mcnValue() == "4988001234567");
    CHECK_EQ(codes.isrcs.size(), 2u);
    CHECK(codes.isrc(1) == "JPVI09912345");
    CHECK(codes.isrc(2).empty());
    CHECK(codes.isrc(3).empty());
    CHECK(codes.isrcs.at(3).status == Status::Invalid);
    const std::vector<std::string> lines = codes.logLines();
    CHECK(lines == std::vector<std::string>({"MCN: 4988001234567", "Track  1  ISRC: JPVI09912345",
                                             "Track  3  ISRC: invalid response (ISRC \"bad\")"}));

    cdr::AlbumMetadata album = sampleAlbum();
    album.trackIsrcs = {"OLD", "OLD", "OLD"};
    codes.applyTo(album);
    CHECK(album.mcn == "4988001234567");
    CHECK(album.trackIsrcs == std::vector<std::string>({"JPVI09912345"}));
    CHECK(album.forTrack(1, 3).isrc == "JPVI09912345");
    CHECK(album.forTrack(1, 3).mcn == "4988001234567");
    CHECK(album.forTrack(3, 3).isrc.empty());
    CHECK(album.forTrack(0, 3).isrc.empty() && album.forTrack(0, 3).mcn == "4988001234567");

    // A drive that rejects the command is asked once only.
    FakeDrive old = makeAudioDisc();
    old.subChannelSupported = false;
    cdr::CdDrive oldDrive(old);
    const cdr::DiscCodes none = cdr::readDiscCodes(oldDrive, oldDrive.readToc().tracks);
    CHECK_EQ(old.subChannelCommands, 1);
    CHECK(none.mcn.status == Status::Unsupported);
    CHECK(none.isrcs.at(3).status == Status::Skipped);
    CHECK(none.mcnValue().empty() && none.isrc(1).empty());
    CHECK(none.logLines()[1] == "Track  1  ISRC: not read (command not supported)");
    CHECK(cdr::DiscCodes{}.logLines() == std::vector<std::string>({"MCN / ISRC: not read (disabled)"}));
}

// A drive that throws from the transport (e.g. a USB device that went away)
// must not abort the rip either.
TEST(read_disc_codes_survives_throwing_transport) {
    struct Throwing : cdr::ScsiTransport {
        cdr::ScsiResult execute(const uint8_t*, size_t, void*, size_t, cdr::DataDirection, unsigned) override {
            throw std::runtime_error("usb gone");
        }
    } transport;
    cdr::CdDrive drive(transport);
    std::vector<cdr::Track> tracks = {{1, 0, 300}};
    const cdr::DiscCodes codes = cdr::readDiscCodes(drive, tracks);
    CHECK(codes.mcn.status == Status::Failed && codes.mcn.detail == "usb gone");
    CHECK(codes.isrcs.at(1).status == Status::Failed);
}

TEST(cue_sheet_catalog_and_isrc) {
    std::vector<cdr::Track> tracks = {{1, 0, 300}, {2, 300, 150}, {3, 450, 300}};
    tracks[1].preEmphasis = true;
    cdr::AlbumMetadata album;
    album.artist = "Artist";
    album.mcn = "4988001234567";
    album.trackIsrcs = {"JPVI09912345", "JPVI09912346", "garbage"};
    const std::string cue = cdr::formatCueSheet(album, cdr::singleFileCueTracks(tracks, "a.flac", album));
    // CATALOG at disc level; ISRC after TRACK (and FLAGS), before INDEX, as
    // metaflac --export-cuesheet-to writes them. Invalid codes are left out.
    CHECK(cue ==
          "REM COMMENT \"cdreader\"\r\n"
          "CATALOG 4988001234567\r\n"
          "PERFORMER \"Artist\"\r\n"
          "FILE \"a.flac\" WAVE\r\n"
          "  TRACK 01 AUDIO\r\n"
          "    PERFORMER \"Artist\"\r\n"
          "    ISRC JPVI09912345\r\n"
          "    INDEX 01 00:00:00\r\n"
          "  TRACK 02 AUDIO\r\n"
          "    PERFORMER \"Artist\"\r\n"
          "    FLAGS PRE\r\n"
          "    ISRC JPVI09912346\r\n"
          "    INDEX 01 00:04:00\r\n"
          "  TRACK 03 AUDIO\r\n"
          "    PERFORMER \"Artist\"\r\n"
          "    INDEX 01 00:06:00\r\n");
    keepSample("isrc.cue", std::vector<uint8_t>(cue.begin(), cue.end()));
    // Per-track sheets carry them too; an invalid MCN is left out.
    album.mcn = "123";
    const std::string perTrack =
        cdr::formatCueSheet(album, cdr::perTrackCueTracks({tracks[0]}, {"01.flac"}, album));
    CHECK(perTrack.find("CATALOG") == std::string::npos);
    CHECK(perTrack.find("  TRACK 01 AUDIO\r\n    PERFORMER \"Artist\"\r\n    ISRC JPVI09912345\r\n    INDEX 01") !=
          std::string::npos);
}

TEST(flac_cuesheet_block_mcn_and_isrc) {
    cdr::EmbeddedCueSheet cue = sampleEmbeddedCue();
    cue.mcn = "4988001234567";
    cue.tracks[0].isrc = "JPVI09912345";
    cue.tracks[2].isrc = "JPVI0991234X";  // invalid: left out
    const std::vector<uint8_t> block = cdr::flac::cueSheet(cue, 100 * 588);
    CHECK_EQ(block.size(), size_t(396 + 3 * 48 + 36));
    // 13 ASCII digits, then NUL bytes up to 128.
    CHECK(str(block, 0, 13) == "4988001234567");
    CHECK(std::all_of(block.begin() + 13, block.begin() + 128, [](uint8_t b) { return b == 0; }));
    const ParsedCueSheet c = parseCueSheet(block);
    CHECK(c.mcn == "4988001234567");
    CHECK(c.isCd);
    CHECK_EQ(c.tracks.size(), 4u);
    if (c.tracks.size() == 4) {
        CHECK(c.tracks[0].isrc == "JPVI09912345");
        CHECK(c.tracks[1].isrc.empty());
        CHECK(c.tracks[2].isrc.empty());
        CHECK(c.tracks[3].isrc.empty());  // lead-out
    }
    // The ISRC is the 12 bytes after the track offset and number.
    const size_t track1 = 396;
    CHECK(str(block, track1 + 9, 12) == "JPVI09912345");
    CHECK_EQ(block[track1 + 21], 0);  // flags follow unchanged
    // An invalid MCN is not written.
    cue.mcn = "0000000000000";
    const std::vector<uint8_t> noMcn = cdr::flac::cueSheet(cue, 100 * 588);
    CHECK(std::all_of(noMcn.begin(), noMcn.begin() + 128, [](uint8_t b) { return b == 0; }));
}

TEST(tags_carry_isrc_and_mcn) {
    cdr::TrackMetadata m = sampleAlbum().forTrack(2, 3);
    m.isrc = "JPVI09912345";
    m.mcn = "4988001234567";
    const std::vector<uint8_t> vc = cdr::vorbisComment(m, "v");
    const std::string text(vc.begin(), vc.end());
    CHECK(text.find("ISRC=JPVI09912345") != std::string::npos);
    CHECK(text.find("BARCODE=4988001234567") != std::string::npos);

    const std::vector<uint8_t> tag = cdr::id3v2Tag(m);
    const auto f = id3Frames(tag, 0);
    CHECK(fieldValue(f, "TSRC") == "JPVI09912345");
    int barcode = 0;
    for (const auto& [id, value] : f)
        barcode += id == "TXXX" && value == std::string("BARCODE") + '\0' + "4988001234567";
    CHECK_EQ(barcode, 1);

    // RIFF INFO has no ISRC field ("ISRC" there means "source"): unchanged.
    cdr::TrackMetadata plain = m;
    plain.isrc.clear();
    plain.mcn.clear();
    CHECK(cdr::riffInfoChunk(m) == cdr::riffInfoChunk(plain));

    // Without codes nothing is added.
    const std::vector<uint8_t> none = cdr::vorbisComment(plain, "v");
    const std::string noneText(none.begin(), none.end());
    CHECK(noneText.find("ISRC=") == std::string::npos && noneText.find("BARCODE=") == std::string::npos);
    CHECK(fieldValue(id3Frames(cdr::id3v2Tag(plain), 0), "TSRC") == "<missing>");
}

TEST(flac_rip_tags_isrc_per_track) {
    // ISRCs read from the fake drive end up in each track's FLAC tags.
    FakeDrive fake = makeAudioDisc();
    fake.mcn = "4988001234567";
    fake.isrcs[2] = "JPVI09912346";
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::AlbumMetadata album;
    cdr::readDiscCodes(drive, toc.tracks).applyTo(album);
    cdr::Ripper ripper(drive, toc, {});
    for (int n : {1, 2}) {
        const std::filesystem::path path =
            cdr_test::testTempDir() / ("cdreader_isrc_" + std::to_string(n) + ".flac");
        cdr::FlacWriter writer;
        writer.open(path, album.forTrack(n, 3));
        ripper.ripTrack(*toc.findTrack(n), [&](const uint8_t* p, size_t b) { writer.write(p, b); });
        writer.close();
        const DecodedFlac d = decodeFlac(readFile(path));
        std::filesystem::remove(path);
        const bool hasIsrc = std::find(d.comments.begin(), d.comments.end(), "ISRC=JPVI09912346") != d.comments.end();
        CHECK_EQ(hasIsrc, n == 2);
        CHECK(std::find(d.comments.begin(), d.comments.end(), "BARCODE=4988001234567") != d.comments.end());
    }
}

// --- Pregap / index / HTOA detection from the Q sub-channel (#25) ----------------

namespace {

FakeDrive makeDisc(const std::vector<uint32_t>& starts, uint32_t leadOut) {
    std::vector<FakeDrive::FakeTrack> tracks;
    for (uint32_t s : starts) tracks.push_back({s, false});
    return FakeDrive(tracks, leadOut);
}

uint8_t toBcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }

// A mode 1 Q frame with CRC: track / index, relative and absolute MSF.
std::vector<uint8_t> qBytes(int track, int index, int rm, int rs, int rf, int am, int as, int af,
                            uint8_t control = 0) {
    std::vector<uint8_t> q = {uint8_t(control << 4 | 1), toBcd(track), toBcd(index), toBcd(rm), toBcd(rs), toBcd(rf), 0,
                              toBcd(am), toBcd(as), toBcd(af), 0, 0};
    const uint16_t crc = uint16_t(cdr::subQCrc16(q.data(), 10) ^ 0xFFFF);
    q[10] = uint8_t(crc >> 8);
    q[11] = uint8_t(crc);
    return q;
}

cdr::GapDetectionOptions forced(cdr::QSource source) {
    cdr::GapDetectionOptions o;
    o.source = source;
    return o;
}

}  // namespace

TEST(subq_crc_and_formatted_q) {
    const char* check = "123456789";
    CHECK_EQ(cdr::subQCrc16(reinterpret_cast<const uint8_t*>(check), 9), 0x31C3);  // CRC-16/XMODEM check value

    // Track 2, INDEX 00, 1 s 74 frames before INDEX 01, at 03:02:10 absolute.
    std::vector<uint8_t> f = qBytes(2, 0, 0, 1, 74, 3, 2, 10);
    f.resize(16, 0);
    cdr::QFrame q = cdr::parseFormattedQ(f.data());
    CHECK(q.usable());
    CHECK(q.crc == cdr::QFrame::Crc::Valid);
    CHECK_EQ(q.track, 2);
    CHECK_EQ(q.index, 0);
    CHECK_EQ(q.relative, -(75 + 74));
    CHECK_EQ(q.absoluteLba, (3 * 60 + 2) * 75 + 10 - 150);
    CHECK_EQ(q.key(), 200);

    // Bit error: the CRC catches it.
    std::vector<uint8_t> bad = f;
    bad[8] ^= 0x01;
    CHECK(cdr::parseFormattedQ(bad.data()).crc == cdr::QFrame::Crc::Invalid);
    CHECK(!cdr::parseFormattedQ(bad.data()).usable());
    // A drive that does not report the CRC (bytes 10, 11 zero): not checked.
    std::vector<uint8_t> noCrc = f;
    noCrc[10] = noCrc[11] = 0;
    CHECK(cdr::parseFormattedQ(noCrc.data()).crc == cdr::QFrame::Crc::Absent);
    CHECK(cdr::parseFormattedQ(noCrc.data()).usable());
    // Not BCD / out of range: unusable.
    noCrc[4] = 0x6A;
    CHECK(!cdr::parseFormattedQ(noCrc.data()).usable());
    noCrc[4] = 0x60;  // 60 seconds
    CHECK(!cdr::parseFormattedQ(noCrc.data()).usable());

    // Mode 2 (MCN) frames carry no position.
    std::vector<uint8_t> mcn(16, 0);
    mcn[0] = 0x02;
    mcn[1] = 0x49;
    q = cdr::parseFormattedQ(mcn.data());
    CHECK(q.parsed && q.adr == 2 && !q.usable());

    // Data track control bits, lead-out (AAh), track 1 INDEX 01 at LBA 0.
    std::vector<uint8_t> lo = qBytes(1, 1, 0, 0, 5, 40, 0, 0, 0x4);
    lo[1] = 0xAA;
    q = cdr::parseQ(lo.data(), false);
    CHECK(q.usable() && q.track == 170 && q.control == 0x4 && q.relative == 5);
    q = cdr::parseQ(qBytes(1, 1, 0, 0, 0, 0, 2, 0).data(), true);
    CHECK(q.usable() && q.absoluteLba == 0 && q.key() == 101);
}

TEST(subq_raw_pw_deinterleave) {
    const std::vector<uint8_t> q = qBytes(12, 1, 1, 2, 3, 45, 6, 7);
    uint8_t raw[96];
    for (size_t j = 0; j < 96; ++j) raw[j] = uint8_t(j * 13 + 5);  // R-W noise
    cdr::interleaveQ(q.data(), true, raw);
    // Byte 0 of Q is 01h: bit 6 of the 8th byte only; P = 1 (pause) everywhere.
    for (size_t j = 0; j < 8; ++j) CHECK_EQ((raw[j] >> 6) & 1, j == 7 ? 1 : 0);
    CHECK(std::all_of(raw, raw + 96, [](uint8_t b) { return (b & 0x80) != 0; }));
    CHECK_EQ(raw[20] & 0x3F, uint8_t(20 * 13 + 5) & 0x3F);  // R-W untouched
    uint8_t back[12];
    cdr::deinterleaveQ(raw, back);
    CHECK(std::equal(back, back + 12, q.begin()));
    cdr::QFrame f = cdr::parseRawPwQ(raw);
    CHECK(f.usable() && f.crc == cdr::QFrame::Crc::Valid && f.track == 12 && f.index == 1);
    CHECK_EQ(f.absoluteLba, (45 * 60 + 6) * 75 + 7 - 150);
    raw[50] ^= 0x40;  // one Q bit flipped
    CHECK(cdr::parseRawPwQ(raw).crc == cdr::QFrame::Crc::Invalid);
}

TEST(subq_current_position_response) {
    uint8_t r[16] = {0, 0x15, 0, 12, 0x01, 0x10, 3, 0, 0, 0, 0x3A, 0x98, 0xFF, 0xFF, 0xFF, 0xFE};
    cdr::QFrame f = cdr::parseCurrentPosition(r, sizeof r);
    CHECK(f.usable() && f.crc == cdr::QFrame::Crc::Absent);
    CHECK_EQ(f.track, 3);
    CHECK_EQ(f.index, 0);
    CHECK_EQ(f.absoluteLba, 15000);
    CHECK_EQ(f.relative, -2);
    CHECK(!cdr::parseCurrentPosition(r, 12).usable());  // short
    r[4] = 0x02;
    CHECK(!cdr::parseCurrentPosition(r, sizeof r).usable());  // other format
}

TEST(read_cd_with_subchannel_cdb_and_frames) {
    FakeDrive fake = makeDisc({0, 300, 450}, 750);
    fake.pregaps[2] = 20;
    cdr::CdDrive drive(fake);
    std::vector<uint8_t> buf(2 * (2352 + 16));
    CHECK(drive.readAudioWithSubChannel(279, 2, cdr::SubChannelSelection::FormattedQ, buf.data()).ok());
    CHECK(fake.lastReadCdCdb ==
          std::vector<uint8_t>({0xBE, 0x04, 0, 0, 0x01, 0x17, 0, 0, 2, 0x10, 0x02, 0}));
    CHECK_EQ(fake.subQReads, 1);
    CHECK_EQ(fake.readCommands, 0);
    // Audio data as usual, Q after each sector: 279 is track 1, 280 the pregap of track 2.
    CHECK_EQ(buf[0], FakeDrive::sampleByte(279, 0));
    const cdr::QFrame a = cdr::parseFormattedQ(buf.data() + 2352);
    const cdr::QFrame b = cdr::parseFormattedQ(buf.data() + 2368 + 2352);
    CHECK(a.key() == 101 && a.absoluteLba == 279 && a.relative == 279);
    CHECK(b.key() == 200 && b.absoluteLba == 280 && b.relative == -20);
    CHECK_EQ(buf[2368], FakeDrive::sampleByte(280, 0));

    std::vector<uint8_t> raw(2352 + 96);
    CHECK(drive.readAudioWithSubChannel(300, 1, cdr::SubChannelSelection::RawPW, raw.data()).ok());
    CHECK_EQ(fake.lastReadCdCdb[10], 0x01);
    CHECK(cdr::parseRawPwQ(raw.data() + 2352).key() == 201);
    fake.rawSubChannelSupported = false;
    const cdr::ScsiResult r = drive.readAudioWithSubChannel(300, 1, cdr::SubChannelSelection::RawPW, raw.data());
    CHECK(!r.ok() && r.sense.key == 0x5);
}

TEST(gaps_detected_for_various_pregap_lengths) {
    // 0, 1, 150 (2 s), 157 (2 s + 7), 1000 and 2999 (all but the first sector
    // of the previous track) sectors.
    const std::vector<uint32_t> starts = {0, 3000, 6000, 9000, 12000, 15000, 18000};
    const std::map<int, uint32_t> pregaps = {{3, 1}, {4, 150}, {5, 157}, {6, 1000}, {7, 2999}};
    for (cdr::QSource source :
         {cdr::QSource::Auto, cdr::QSource::FormattedQ, cdr::QSource::RawPW, cdr::QSource::CurrentPosition}) {
        FakeDrive fake = makeDisc(starts, 21000);
        fake.pregaps = pregaps;
        cdr::CdDrive drive(fake);
        const cdr::Toc toc = drive.readToc();
        const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc, forced(source));
        CHECK(gaps.status == cdr::DiscGaps::Status::Detected);
        CHECK(gaps.source == (source == cdr::QSource::Auto ? cdr::QSource::FormattedQ : source));
        CHECK_EQ(gaps.tracks.size(), 7u);
        CHECK(!gaps.hasHtoa());
        for (int n = 1; n <= 7; ++n) {
            const auto it = pregaps.find(n);
            CHECK_EQ(gaps.pregap(n), it == pregaps.end() ? 0u : it->second);
            CHECK(gaps.find(n) && gaps.find(n)->detected());
            CHECK(gaps.laterIndexes(n).empty());
        }
        CHECK_EQ(gaps.find(5)->index00Lba(), 12000u - 157);
        // A few dozen reads per track (READ SUB-CHANNEL needs two commands per sector).
        const unsigned perTrack = source == cdr::QSource::CurrentPosition ? 80 : 40;
        CHECK(gaps.reads <= 7 * perTrack);
        CHECK_EQ(gaps.reads, unsigned(fake.subQReads + fake.subChannelCommands + fake.readCommands));
    }
}

TEST(gaps_fall_back_when_formatted_q_is_unsupported) {
    FakeDrive fake = makeDisc({0, 3000, 6000}, 9000);
    fake.pregaps = {{2, 150}, {3, 33}};
    fake.formattedQSupported = false;
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    CHECK(gaps.source == cdr::QSource::RawPW);
    CHECK(gaps.pregap(2) == 150 && gaps.pregap(3) == 33);
    // Neither READ CD selection: READ SUB-CHANNEL after each read.
    fake.rawSubChannelSupported = false;
    gaps = cdr::detectGaps(drive, toc);
    CHECK(gaps.source == cdr::QSource::CurrentPosition);
    CHECK(gaps.pregap(2) == 150 && gaps.pregap(3) == 33);
    CHECK(gaps.logLines()[0].find("READ SUB-CHANNEL current position") != std::string::npos);
}

TEST(gaps_unsupported_drive_means_no_gaps) {
    FakeDrive fake = makeDisc({0, 3000, 6000}, 9000);
    fake.pregaps = {{2, 150}};
    fake.formattedQSupported = fake.rawSubChannelSupported = fake.currentPositionSupported = false;
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    CHECK(gaps.status == cdr::DiscGaps::Status::Unsupported);
    CHECK(gaps.tracks.empty());
    CHECK_EQ(gaps.pregap(2), 0u);
    CHECK(gaps.reads <= 4);  // one rejected command per method (+ the READ CD before READ SUB-CHANNEL)
    CHECK(gaps.logLines()[0] ==
          "Gap detection: not possible (the drive does not return Q sub-channel data), no pregaps known");
    // The sheet is the one written without detection.
    const cdr::AlbumMetadata album;
    const std::string cue = cdr::formatCueSheet(album, cdr::singleFileCueTracks(toc.tracks, "a.wav", album, gaps));
    CHECK(cue.find("INDEX 00") == std::string::npos);

    // Q frames that never validate (always corrupt) are as good as none.
    FakeDrive noisy = makeDisc({0, 3000}, 6000);
    noisy.currentPositionSupported = false;
    for (uint32_t s = 0; s < 40; ++s) noisy.badQ[s] = -1;
    cdr::CdDrive noisyDrive(noisy);
    const cdr::DiscGaps none = cdr::detectGaps(noisyDrive, noisyDrive.readToc());
    CHECK(none.status == cdr::DiscGaps::Status::Unsupported);
    CHECK(none.detail == "no usable Q sub-channel frames");
}

TEST(gaps_htoa_detected_and_confirmed) {
    // Track 1 starts 1 min 2 s after LBA 0: a hidden track of 4650 sectors.
    FakeDrive fake = makeDisc({4650, 9000, 12000}, 15000);
    fake.pregaps = {{2, 150}};
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    CHECK_EQ(cdr::htoaSectors(toc), 4650u);
    const cdr::Track h = cdr::htoaTrack(toc);
    CHECK(h.number == 0 && h.startLba == 0 && h.lengthSectors == 4650);
    // The HTOA is readable audio: offset correction reads across it.
    CHECK_EQ(toc.audioRange(*toc.findTrack(1)).begin, 0u);
    CHECK(toc.audioRange(h).begin == 0 && toc.audioRange(h).end == 15000);
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    CHECK(gaps.hasHtoa() && gaps.htoaConfirmed);
    CHECK_EQ(gaps.pregap(1), 4650u);
    CHECK_EQ(gaps.find(1)->index00Lba(), 0u);
    CHECK_EQ(gaps.pregap(2), 150u);
    const std::vector<std::string> log = gaps.logLines();
    CHECK(log.size() == 5);
    if (log.size() == 5) {
        CHECK(log[1] == "HTOA (hidden track before track 1): 01:02.00, LBA 0-4649, confirmed by the Q sub-channel");
        CHECK(log[2] == "Track  1  pregap 01:02.00  INDEX 00 at LBA 0 (HTOA)");
        CHECK(log[3] == "Track  2  pregap 00:02.00  INDEX 00 at LBA 8850");
        CHECK(log[4] == "Track  3  pregap 00:00.00");
    }
    // Known from the TOC without detection.
    const cdr::DiscGaps toc0 = cdr::gapsFromToc(toc);
    CHECK(toc0.status == cdr::DiscGaps::Status::NotRun && toc0.pregap(1) == 4650 && toc0.pregap(2) == 0);
    CHECK(toc0.logLines() == std::vector<std::string>({"Gap detection: not run (disabled)",
                                                        "HTOA (hidden track before track 1): 01:02.00, LBA 0-4649 "
                                                        "(from the TOC)"}));
    // A disc without HTOA, and a mixed-mode disc (data track 1).
    CHECK_EQ(cdr::htoaSectors(makeCddbToc()), 0u);
    FakeDrive mixed({{0, true}, {9000, false}}, 12000);
    cdr::CdDrive mixedDrive(mixed);
    const cdr::Toc mixedToc = mixedDrive.readToc();
    CHECK_EQ(cdr::htoaSectors(mixedToc), 0u);
    const cdr::DiscGaps mixedGaps = cdr::detectGaps(mixedDrive, mixedToc);
    CHECK(mixedGaps.tracks.size() == 1 && mixedGaps.tracks[0].status == cdr::TrackIndexes::Status::Skipped);
    CHECK(mixedGaps.logLines().back() == "Track  2  pregap not searched (follows a data track)");
}

TEST(gaps_later_index_points) {
    FakeDrive fake = makeDisc({0, 3000, 6000, 9000}, 12000);
    fake.pregaps = {{2, 150}, {3, 75}};
    fake.laterIndexes[2] = {3500, 4000, 5800};  // INDEX 02 / 03 / 04 of track 2
    fake.laterIndexes[4] = {11999};             // INDEX 02 in the last sector
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    CHECK(gaps.status == cdr::DiscGaps::Status::Detected);
    CHECK(gaps.laterIndexes(2) == std::vector<uint32_t>({3500, 4000, 5800}));
    CHECK(gaps.laterIndexes(1).empty() && gaps.laterIndexes(3).empty());
    CHECK(gaps.laterIndexes(4) == std::vector<uint32_t>({11999}));
    CHECK(gaps.pregap(3) == 75);
    CHECK(gaps.logLines()[2].find("INDEX 00 at LBA 2850  INDEX 02 at LBA 3500  INDEX 03 at LBA 4000  INDEX 04 at LBA 5800") !=
          std::string::npos);
    // Without the option: one read less per track, no index points.
    cdr::GapDetectionOptions options;
    options.laterIndexes = false;
    const cdr::DiscGaps plain = cdr::detectGaps(drive, toc, options);
    CHECK(plain.laterIndexes(2).empty() && plain.pregap(2) == 150);
    CHECK(plain.reads < gaps.reads);
}

TEST(gaps_survive_noisy_q_frames) {
    // Mode 2 / 3 frames every 7th sector (real discs: about 1 in 100), and
    // corrupt frames (bad CRC) around every boundary, some never readable.
    for (cdr::QSource source : {cdr::QSource::FormattedQ, cdr::QSource::RawPW}) {
        FakeDrive fake = makeDisc({0, 3000, 6000, 9000}, 12000);
        fake.pregaps = {{2, 150}, {3, 1}, {4, 0}};
        fake.otherAdrEvery = 7;
        for (uint32_t b : {2850u, 5999u, 9000u}) {
            for (uint32_t s = b - 4; s <= b + 4; ++s) fake.badQ[s] = s % 2 ? 1 : 2;
            fake.badQ[b - 3] = fake.badQ[b + 2] = -1;
        }
        cdr::CdDrive drive(fake);
        const cdr::Toc toc = drive.readToc();
        const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc, forced(source));
        CHECK(gaps.status == cdr::DiscGaps::Status::Detected);
        CHECK_EQ(gaps.pregap(2), 150u);
        CHECK_EQ(gaps.pregap(3), 1u);
        CHECK_EQ(gaps.pregap(4), 0u);
    }
}

TEST(gaps_wrong_frames_without_crc_are_voted_out) {
    // A drive that reports no CRC returns frames from the wrong side of the
    // boundary once: the confirmation reads catch it and the search repeats
    // with two agreeing reads per position.
    FakeDrive fake = makeDisc({0, 3000, 6000}, 9000);
    fake.pregaps = {{2, 150}, {3, 20}};
    fake.formattedQCrc = false;
    for (uint32_t s = 2780; s < 3000; ++s) fake.wrongQ[s] = 1;
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc, forced(cdr::QSource::FormattedQ));
    CHECK(gaps.status == cdr::DiscGaps::Status::Detected);
    CHECK_EQ(gaps.pregap(2), 150u);
    CHECK_EQ(gaps.pregap(3), 20u);
    // With a CRC the same faults are simply discarded.
    FakeDrive withCrc = makeDisc({0, 3000, 6000}, 9000);
    withCrc.pregaps = fake.pregaps;
    for (uint32_t s = 2780; s < 3000; ++s) withCrc.wrongQ[s] = 1;
    cdr::CdDrive crcDrive(withCrc);
    CHECK_EQ(cdr::detectGaps(crcDrive, toc, forced(cdr::QSource::RawPW)).pregap(2), 150u);
    // No readable frame around the boundary: the track stays unknown (no INDEX 00).
    FakeDrive broken = makeDisc({0, 3000, 6000}, 9000);
    broken.pregaps = fake.pregaps;
    broken.formattedQCrc = false;
    for (uint32_t s = 2840; s <= 2860; ++s) broken.badQ[s] = -1;
    cdr::CdDrive brokenDrive(broken);
    const cdr::DiscGaps unsure = cdr::detectGaps(brokenDrive, toc, forced(cdr::QSource::FormattedQ));
    CHECK(unsure.status == cdr::DiscGaps::Status::Partial);
    CHECK(unsure.find(2) && unsure.find(2)->status == cdr::TrackIndexes::Status::Unknown);
    CHECK_EQ(unsure.pregap(2), 0u);
    CHECK_EQ(unsure.pregap(3), 20u);
}

TEST(gaps_read_budget) {
    FakeDrive fake = makeDisc({0, 3000, 6000, 9000, 12000}, 15000);
    fake.pregaps = {{2, 150}, {3, 150}, {4, 150}, {5, 150}};
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::GapDetectionOptions options;
    options.maxReads = 40;
    std::vector<int> progress;
    options.progress = [&](int track, int last) {
        CHECK_EQ(last, 5);
        progress.push_back(track);
    };
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc, options);
    CHECK(progress == std::vector<int>({1, 2, 3, 4, 5}));
    CHECK(gaps.status == cdr::DiscGaps::Status::Partial);
    CHECK(gaps.reads <= 40);
    CHECK_EQ(fake.subQReads, int(gaps.reads));
    CHECK_EQ(gaps.pregap(2), 150u);
    CHECK(gaps.find(5)->status == cdr::TrackIndexes::Status::Unknown);
    CHECK(gaps.find(5)->detail == "read budget exhausted");
    CHECK_EQ(gaps.pregap(5), 0u);
}

TEST(cue_sheet_single_file_with_gaps_and_htoa) {
    // HTOA of 450 sectors, track 2 with a 2-second pregap and an INDEX 02.
    const std::vector<cdr::Track> tracks = {{1, 450, 1050}, {2, 1500, 750}, {3, 2250, 300}};
    cdr::DiscGaps gaps;
    gaps.status = cdr::DiscGaps::Status::Detected;
    gaps.htoaSectors = 450;
    gaps.tracks.resize(3);
    for (int i = 0; i < 3; ++i) {
        gaps.tracks[size_t(i)].track = i + 1;
        gaps.tracks[size_t(i)].index01Lba = tracks[size_t(i)].startLba;
        gaps.tracks[size_t(i)].status = cdr::TrackIndexes::Status::Detected;
    }
    gaps.tracks[0].pregapSectors = 450;
    gaps.tracks[1].pregapSectors = 150;
    gaps.tracks[1].laterIndexes = {1800};
    cdr::AlbumMetadata album;
    const std::string cue =
        cdr::formatCueSheet(album, cdr::singleFileCueTracks(tracks, "image.wav", album, gaps, true));
    CHECK(cue ==
          "REM COMMENT \"cdreader\"\r\n"
          "FILE \"image.wav\" WAVE\r\n"
          "  TRACK 01 AUDIO\r\n"
          "    INDEX 00 00:00:00\r\n"
          "    INDEX 01 00:06:00\r\n"
          "  TRACK 02 AUDIO\r\n"
          "    INDEX 00 00:18:00\r\n"
          "    INDEX 01 00:20:00\r\n"
          "    INDEX 02 00:24:00\r\n"
          "  TRACK 03 AUDIO\r\n"
          "    INDEX 01 00:30:00\r\n");
    keepSample("gaps_single.cue", std::vector<uint8_t>(cue.begin(), cue.end()));
    // Without the HTOA in the image: positions from track 1's INDEX 01, and a
    // PREGAP line keeps the layout.
    const std::string noHtoa = cdr::formatCueSheet(album, cdr::singleFileCueTracks(tracks, "image.wav", album, gaps));
    CHECK(noHtoa.find("  TRACK 01 AUDIO\r\n    PREGAP 00:06:00\r\n    INDEX 01 00:00:00\r\n") != std::string::npos);
    CHECK(noHtoa.find("    INDEX 00 00:12:00\r\n    INDEX 01 00:14:00\r\n    INDEX 02 00:18:00\r\n") !=
          std::string::npos);
    // A selection starting at track 2: its pregap is not in the image.
    const std::vector<cdr::Track> tail(tracks.begin() + 1, tracks.end());
    const std::string tailCue = cdr::formatCueSheet(album, cdr::singleFileCueTracks(tail, "t.wav", album, gaps));
    CHECK(tailCue.find("INDEX 00") == std::string::npos);
    CHECK(tailCue.find("  TRACK 02 AUDIO\r\n    INDEX 01 00:00:00\r\n    INDEX 02 00:04:00\r\n") != std::string::npos);
    bool threw = false;
    try {
        cdr::singleFileCueTracks(tail, "t.wav", album, gaps, true);  // the HTOA belongs before track 1
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(cue_sheet_per_track_gaps_appended_to_previous) {
    const std::vector<cdr::Track> tracks = {{1, 450, 1050}, {2, 1500, 750}, {3, 2250, 300}};
    cdr::DiscGaps gaps;
    gaps.status = cdr::DiscGaps::Status::Detected;
    gaps.htoaSectors = 450;
    for (int i = 0; i < 3; ++i) {
        cdr::TrackIndexes t;
        t.track = i + 1;
        t.index01Lba = tracks[size_t(i)].startLba;
        t.status = cdr::TrackIndexes::Status::Detected;
        gaps.tracks.push_back(t);
    }
    gaps.tracks[1].pregapSectors = 150;
    gaps.tracks[2].pregapSectors = 32;
    gaps.tracks[2].laterIndexes = {2400};
    cdr::AlbumMetadata album;
    const std::vector<std::string> files = {"01.flac", "02.flac", "03.flac"};
    // EAC's default sheet: the HTOA ripped as track 00, each pregap at the
    // end of the previous file.
    const std::string cue =
        cdr::formatCueSheet(album, cdr::perTrackCueTracks(tracks, files, album, gaps, "00.flac"));
    CHECK(cue ==
          "REM COMMENT \"cdreader\"\r\n"
          "FILE \"00.flac\" WAVE\r\n"
          "  TRACK 01 AUDIO\r\n"
          "    INDEX 00 00:00:00\r\n"
          "FILE \"01.flac\" WAVE\r\n"
          "    INDEX 01 00:00:00\r\n"
          "  TRACK 02 AUDIO\r\n"
          "    INDEX 00 00:12:00\r\n"
          "FILE \"02.flac\" WAVE\r\n"
          "    INDEX 01 00:00:00\r\n"
          "  TRACK 03 AUDIO\r\n"
          "    INDEX 00 00:09:43\r\n"
          "FILE \"03.flac\" WAVE\r\n"
          "    INDEX 01 00:00:00\r\n"
          "    INDEX 02 00:02:00\r\n");
    keepSample("gaps_per_track.cue", std::vector<uint8_t>(cue.begin(), cue.end()));
    // HTOA not ripped: PREGAP for track 1.
    const std::string noHtoa = cdr::formatCueSheet(album, cdr::perTrackCueTracks(tracks, files, album, gaps));
    CHECK(noHtoa.rfind("REM COMMENT \"cdreader\"\r\n"
                       "FILE \"01.flac\" WAVE\r\n"
                       "  TRACK 01 AUDIO\r\n"
                       "    PREGAP 00:06:00\r\n"
                       "    INDEX 01 00:00:00\r\n",
                       0) == 0);
    // Track 2 not ripped: track 3's pregap (in track 2's file) is left out.
    const std::string gap = cdr::formatCueSheet(
        album, cdr::perTrackCueTracks({tracks[0], tracks[2]}, {"01.flac", "03.flac"}, album, gaps, "00.flac"));
    CHECK(gap.find("FILE \"03.flac\" WAVE\r\n  TRACK 03 AUDIO\r\n    INDEX 01 00:00:00\r\n    INDEX 02 00:02:00\r\n") !=
          std::string::npos);
    // Without gap information the sheet is unchanged.
    CHECK(cdr::formatCueSheet(album, cdr::perTrackCueTracks(tracks, files, album)) ==
          cdr::formatCueSheet(album, cdr::perTrackCueTracks(tracks, files, album, cdr::DiscGaps{})));
}

TEST(flac_cuesheet_block_with_index_00) {
    cdr::EmbeddedCueSheet cue = sampleEmbeddedCue();  // tracks at 0, 30, 75; 100 sectors
    cue.tracks[0].hasIndex00 = true;  // an HTOA of 10 sectors: INDEX 00 at 0, INDEX 01 at 10
    cue.tracks[0].index00Sectors = 0;
    cue.tracks[0].startSectors = 10;
    cue.tracks[1].hasIndex00 = true;  // pregap of 5 sectors
    cue.tracks[1].index00Sectors = 25;
    cue.tracks[2].laterIndexes = {80, 90};
    const std::vector<uint8_t> block = cdr::flac::cueSheet(cue, 100 * 588);
    CHECK_EQ(block.size(), size_t(396 + 3 * 36 + 7 * 12 + 36));
    const ParsedCueSheet c = parseCueSheet(block);
    CHECK_EQ(c.tracks.size(), 4u);
    if (c.tracks.size() == 4) {
        // A track starts at its first index point; index offsets are relative to it.
        CHECK_EQ(c.tracks[0].offset, 0u);
        CHECK_EQ(c.tracks[0].indexes.size(), 2u);
        CHECK(c.tracks[0].indexes[0].number == 0 && c.tracks[0].indexes[0].offset == 0);
        CHECK(c.tracks[0].indexes[1].number == 1 && c.tracks[0].indexes[1].offset == 10 * 588);
        CHECK_EQ(c.tracks[1].offset, uint64_t(25 * 588));
        CHECK(c.tracks[1].indexes.size() == 2 && c.tracks[1].indexes[1].offset == 5 * 588);
        CHECK_EQ(c.tracks[2].offset, uint64_t(75 * 588));
        CHECK_EQ(c.tracks[2].indexes.size(), 3u);
        if (c.tracks[2].indexes.size() == 3) {
            CHECK(c.tracks[2].indexes[1].number == 2 && c.tracks[2].indexes[1].offset == 5 * 588);
            CHECK(c.tracks[2].indexes[2].number == 3 && c.tracks[2].indexes[2].offset == 15 * 588);
        }
    }
    CHECK_EQ(cdr::flac::cueSheetLeadOutOffsetPosition(cue), block.size() - 36);
    CHECK_EQ(be64At(block, cdr::flac::cueSheetLeadOutOffsetPosition(cue)), uint64_t(100 * 588));
    // An INDEX 00 in another file (per-track sheet) is not part of the block.
    cue.tracks[1].index00File = "other.flac";
    CHECK_EQ(parseCueSheet(cdr::flac::cueSheet(cue, 100 * 588)).tracks[1].offset, uint64_t(30 * 588));
}

// Whole disc with an HTOA, pregaps and read offset correction into one FLAC:
// the image equals the disc from LBA 0 (shifted by the offset), every INDEX
// points where the Q sub-channel put it, and the lead-out written on close
// lands after the extra index points.
TEST(single_file_image_with_htoa_and_gaps) {
    for (int offset : {0, 667, -1206}) {
        FakeDrive fake = makeDisc({300, 900, 1200}, 1500);
        fake.pregaps = {{2, 150}, {3, 7}};
        fake.laterIndexes[2] = {1000};
        cdr::CdDrive drive(fake);
        const cdr::Toc toc = drive.readToc();
        const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
        CHECK(gaps.htoaConfirmed && gaps.pregap(2) == 150 && gaps.pregap(3) == 7);
        cdr::RipOptions options;
        options.readOffsetSamples = offset;
        cdr::Ripper ripper(drive, toc, options);
        cdr::AlbumMetadata album;
        cdr::EmbeddedCueSheet cue;
        cue.tracks = cdr::singleFileCueTracks(toc.tracks, "image.flac", album, gaps, true);
        cue.totalSectors = toc.leadOutLba;
        cue.text = cdr::formatCueSheet(album, cue.tracks);
        CHECK(cue.text.find("    INDEX 00 00:00:00\r\n    INDEX 01 00:04:00\r\n") != std::string::npos);
        CHECK(cue.text.find("    INDEX 00 00:10:00\r\n    INDEX 01 00:12:00\r\n    INDEX 02 00:13:25\r\n") !=
              std::string::npos);

        const std::filesystem::path path = cdr_test::testTempDir() / "cdreader_htoa_image.flac";
        cdr::FlacWriter writer;
        writer.setEmbeddedCueSheet(cue);
        writer.open(path, album.forTrack(0, 3));
        std::vector<uint8_t> concatenated;
        std::vector<cdr::Track> parts = {cdr::htoaTrack(toc)};
        parts.insert(parts.end(), toc.tracks.begin(), toc.tracks.end());
        for (const cdr::Track& t : parts)
            ripper.ripTrack(t, [&](const uint8_t* p, size_t n) {
                writer.write(p, n);
                concatenated.insert(concatenated.end(), p, p + n);
            });
        writer.close();
        const std::vector<uint8_t> file = readFile(path);
        std::filesystem::remove(path);

        CHECK(concatenated == expectedWithOffset(0, 1500, offset, 0, 1500));
        const DecodedFlac d = decodeFlac(file);
        CHECK(d.pcm == concatenated);
        const ParsedCueSheet c = parseCueSheet(flacBlock(file, 5));
        CHECK_EQ(c.tracks.size(), 4u);
        if (c.tracks.size() == 4) {
            CHECK(c.tracks[0].offset == 0 && c.tracks[0].indexes.size() == 2);
            CHECK_EQ(c.tracks[0].indexes[1].offset, uint64_t(300 * 588));
            CHECK_EQ(c.tracks[1].offset, uint64_t(750 * 588));
            CHECK_EQ(c.tracks[1].indexes.size(), 3u);
            CHECK_EQ(c.tracks[2].offset, uint64_t(1193 * 588));
            CHECK_EQ(c.tracks[3].offset, d.totalSamples);
        }
        // Each track's INDEX 01 is where its own rip starts.
        for (const cdr::CueTrack& t : cue.tracks) {
            const OffsetRip alone = ripWithOffset(fake, t.number, offset);
            const size_t at = size_t(t.startSectors) * cdr::kSectorBytes;
            CHECK(std::equal(alone.bytes.begin(), alone.bytes.end(), concatenated.begin() + ptrdiff_t(at)));
        }
    }
}

// #25 with the other formats: the image with an HTOA and pregaps (CUE sheet
// as in single_file_image_with_htoa_and_gaps) as Ogg FLAC carries the same
// CUESHEET block as the native FLAC (index points 0 / 1 / 2, lead-out patched
// on close after the extra index points); ALAC (no embedded CUE sheet) just
// holds the image. One sector less than announced, so the lead-out moves.
TEST(single_file_htoa_gaps_in_ogg_flac_and_alac) {
    FakeDrive fake = makeDisc({300, 900, 1200}, 1500);
    fake.pregaps = {{2, 150}, {3, 7}};
    fake.laterIndexes[2] = {1000};
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    cdr::Ripper ripper(drive, toc, cdr::RipOptions{});
    cdr::AlbumMetadata album;
    cdr::EmbeddedCueSheet cue;
    cue.tracks = cdr::singleFileCueTracks(toc.tracks, "image.oga", album, gaps, true);
    cue.totalSectors = toc.leadOutLba;
    cue.text = cdr::formatCueSheet(album, cue.tracks);
    std::vector<uint8_t> image;
    std::vector<cdr::Track> parts = {cdr::htoaTrack(toc)};
    parts.insert(parts.end(), toc.tracks.begin(), toc.tracks.end());
    for (const cdr::Track& t : parts)
        ripper.ripTrack(t, [&](const uint8_t* p, size_t n) { image.insert(image.end(), p, p + n); });
    CHECK_EQ(image.size(), size_t(1500) * cdr::kSectorBytes);
    image.resize(image.size() - cdr::kSectorBytes);

    auto encode = [&](const std::string& format, const std::string& name) {
        std::unique_ptr<cdr::AudioWriter> w = cdr::createAudioWriter(format);
        if (w->canEmbedCueSheet()) w->setEmbeddedCueSheet(cue);
        const std::filesystem::path path = cdr_test::testTempDir() / name;
        w->open(path, album.forTrack(0, 3));
        for (size_t pos = 0; pos < image.size(); pos += 4000)
            w->write(image.data() + pos, std::min<size_t>(4000, image.size() - pos));
        w->close();
        std::vector<uint8_t> file = readFile(path);
        std::filesystem::remove(path);
        return file;
    };
    const std::vector<uint8_t> native = encode("flac", "cdreader_gaps_image.flac");
    const std::vector<uint8_t> ogg = encode("oggflac", "cdreader_gaps_image.oga");
    keepSample("gaps_htoa_image.oga", ogg);
    const OggFlac o = demuxOggFlac(ogg);
    CHECK_EQ(o.headerPackets, 3u);
    if (!o.native.empty()) {
        const DecodedFlac d = decodeFlac(o.native);
        CHECK(d.pcm == image);
        const std::vector<uint8_t> block = flacBlock(o.native, 5);
        CHECK(block == flacBlock(native, 5));
        const ParsedCueSheet c = parseCueSheet(block);
        CHECK_EQ(c.tracks.size(), 4u);
        if (c.tracks.size() == 4) {
            CHECK(c.tracks[0].offset == 0 && c.tracks[0].indexes.size() == 2);
            CHECK(c.tracks[0].indexes[0].number == 0 && c.tracks[0].indexes[1].offset == 300 * 588);
            CHECK_EQ(c.tracks[1].offset, uint64_t(750 * 588));
            CHECK_EQ(c.tracks[1].indexes.size(), 3u);
            CHECK_EQ(c.tracks[2].offset, uint64_t(1193 * 588));
            CHECK_EQ(c.tracks[3].number, 170);
            CHECK_EQ(c.tracks[3].offset, uint64_t(1499 * 588));  // the written length, not the announced one
        }
        const std::string tag = "CUESHEET=" + cdr::flac::cueSheetTagText(cue.text);
        CHECK(std::find(d.comments.begin(), d.comments.end(), tag) != d.comments.end());
    }

    CHECK(!cdr::createAudioWriter("alac")->canEmbedCueSheet());
    const DecodedM4a m = decodeM4a(encode("alac", "cdreader_gaps_image.m4a"));
    CHECK(m.pcm == image);
}

// --- C2 error pointers (#33) -------------------------------------------------

namespace {

FakeDrive makeC2Disc() {
    FakeDrive fake = makeAudioDisc();
    fake.c2Supported = true;
    return fake;
}

struct C2Rip {
    cdr::TrackRipResult result;
    std::vector<uint8_t> bytes;
    bool c2Active = false;
};

C2Rip ripC2(FakeDrive& fake, int track, cdr::RipOptions options) {
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, options);
    C2Rip out;
    out.result = ripper.ripTrack(*toc.findTrack(track), [&](const uint8_t* p, size_t n) {
        out.bytes.insert(out.bytes.end(), p, p + n);
    });
    out.c2Active = ripper.c2Active();
    return out;
}

cdr::RipOptions c2Options(int retries = 3, bool verify = false, int offset = 0) {
    cdr::RipOptions o;
    o.maxRetries = retries;
    o.verify = verify;
    o.readOffsetSamples = offset;
    o.useC2 = true;
    return o;
}

// A permanent C2 error over bytes [first, first + count) of a sector, with
// different wrong data on every read.
FakeDrive::C2Fault c2Fault(size_t first = 1000, size_t count = 16, int reads = -1) {
    FakeDrive::C2Fault f;
    f.reads = reads;
    f.firstByte = first;
    f.byteCount = count;
    return f;
}

}  // namespace

TEST(c2_read_cd_cdb_and_layout) {
    FakeDrive fake = makeC2Disc();
    cdr::CdDrive drive(fake);
    std::vector<uint8_t> buf(2 * cdr::kSectorWithC2Bytes, 0xEE);
    CHECK(drive.readAudioWithC2(0x0123, 2, buf.data()).ok());
    CHECK(fake.lastAudioCdb == (std::vector<uint8_t>{0xBE, 0x04, 0x00, 0x00, 0x01, 0x23, 0, 0, 2, 0x12, 0x00, 0}));
    CHECK_EQ(cdr::kSectorWithC2Bytes, size_t(2646));
    CHECK(cdr::kMaxSectorsPerC2Read * cdr::kSectorWithC2Bytes <= 65536u);
    // Audio, then 294 bytes of C2 bits per sector (none on a clean disc).
    CHECK_EQ(buf[0], FakeDrive::sampleByte(0x0123, 0));
    CHECK_EQ(buf[cdr::kSectorWithC2Bytes], FakeDrive::sampleByte(0x0124, 0));
    CHECK(std::all_of(buf.begin() + cdr::kSectorBytes, buf.begin() + cdr::kSectorWithC2Bytes,
                      [](uint8_t b) { return b == 0; }));

    // C2 bits: one per byte, MSB first.
    fake.c2Faults[10] = c2Fault(9, 2);
    std::vector<uint8_t> one(cdr::kSectorWithC2Bytes);
    CHECK(drive.readAudioWithC2(10, 1, one.data()).ok());
    CHECK_EQ(one[cdr::kSectorBytes + 1], 0x60);  // bytes 9 and 10
    CHECK_EQ(one[cdr::kSectorBytes + 0], 0x00);

    // Plain reads keep byte 9 = 0x10 (user data only).
    CHECK(drive.readAudio(20, 1, one.data()).ok());
    CHECK_EQ(fake.lastAudioCdb[9], 0x10);
    CHECK_EQ(fake.c2ReadCommands, 2);
}

TEST(c2_short_read_is_flagged) {
    FakeDrive fake = makeC2Disc();
    fake.c2IgnoresErrorField = true;
    cdr::CdDrive drive(fake);
    std::vector<uint8_t> buf(cdr::kSectorWithC2Bytes);
    const cdr::ScsiResult r = drive.readAudioWithC2(0, 1, buf.data());
    CHECK(!r.ok());
    CHECK(r.shortRead);
    CHECK(r.describe().find("short read") != std::string::npos);
}

TEST(mode_sense_capabilities) {
    FakeDrive fake = makeC2Disc();
    cdr::CdDrive drive(fake);
    cdr::DriveCapabilities caps = drive.readCapabilities();
    CHECK(caps.valid);
    CHECK(caps.c2Pointers);
    fake.c2Supported = false;
    caps = drive.readCapabilities();
    CHECK(caps.valid);
    CHECK(!caps.c2Pointers);
    fake.modeSenseSupported = false;
    caps = drive.readCapabilities();
    CHECK(!caps.valid);
    CHECK(!caps.c2Pointers);
    CHECK(caps.error.find("MODE SENSE failed") == 0);

    // A block descriptor before the page, the PS bit set, byte 5 bit 4.
    uint8_t data[8 + 8 + 8] = {};
    data[1] = sizeof data - 2;
    data[7] = 8;
    data[16] = 0x80 | 0x2A;
    data[17] = 6;
    data[21] = 0x10;
    caps = cdr::DriveCapabilities::parse(data, sizeof data);
    CHECK(caps.valid && caps.c2Pointers);
    data[21] = 0xEF;  // every other bit
    CHECK(!cdr::DriveCapabilities::parse(data, sizeof data).c2Pointers);
    data[16] = 0x0E;  // another page
    CHECK(!cdr::DriveCapabilities::parse(data, sizeof data).valid);
    data[16] = 0x2A;
    CHECK(!cdr::DriveCapabilities::parse(data, 20).valid);  // truncated
    data[1] = 10;  // mode data length says the page is cut off
    CHECK(!cdr::DriveCapabilities::parse(data, sizeof data).valid);
    CHECK(!cdr::DriveCapabilities::parse(data, 4).valid);
}

TEST(c2_availability_and_log_line) {
    FakeDrive fake = makeC2Disc();
    cdr::CdDrive drive(fake);
    cdr::C2Availability a = cdr::checkC2(drive, true);
    CHECK(a.usable());
    CHECK(a.logLine() == "C2 pointers: supported");
    a = cdr::checkC2(drive, false);
    CHECK(a.mode == cdr::C2Availability::Mode::Disabled);
    CHECK(a.logLine() == "C2 pointers: disabled");
    fake.c2Supported = false;
    a = cdr::checkC2(drive, true);
    CHECK(!a.usable());
    CHECK(a.logLine() == "C2 pointers: not supported");
    fake.modeSenseSupported = false;
    a = cdr::checkC2(drive, true);
    CHECK(a.mode == cdr::C2Availability::Mode::NotSupported);
    CHECK(a.logLine().rfind("C2 pointers: not supported (MODE SENSE failed: ", 0) == 0);
}

// Without C2 (disabled, or a drive without support) the ripper sends exactly
// the plain reads it always did; with C2 on a clean disc the audio is the same.
TEST(c2_disabled_or_clean_gives_identical_output) {
    FakeDrive plain = makeAudioDisc();
    plain.c2Faults[310] = c2Fault();  // corrupt data the drive would flag
    const C2Rip reference = ripC2(plain, 2, cdr::RipOptions{});
    CHECK_EQ(plain.c2ReadCommands, 0);
    const int plainReads = plain.readCommands;
    CHECK_EQ(plainReads, int((150 + cdr::kMaxSectorsPerRead - 1) / cdr::kMaxSectorsPerRead));

    FakeDrive off = makeC2Disc();
    off.c2Faults[310] = c2Fault();
    cdr::RipOptions disabled = c2Options();
    disabled.useC2 = false;
    const C2Rip a = ripC2(off, 2, disabled);
    CHECK(a.bytes == reference.bytes);
    CHECK_EQ(a.result.crc32, reference.result.crc32);
    CHECK_EQ(off.readCommands, plainReads);
    CHECK_EQ(off.c2ReadCommands, 0);
    CHECK(!a.result.c2);
    CHECK(a.result.clean());  // nothing noticed without C2
    CHECK(cdr::c2LogLines(a.result).empty());

    FakeDrive clean = makeC2Disc();
    const C2Rip b = ripC2(clean, 2, c2Options());
    CHECK(b.bytes == expectedTrackData(300, 150));
    CHECK(b.result.c2);
    CHECK(b.c2Active);
    CHECK_EQ(b.result.c2ErrorSectors, 0u);
    CHECK_EQ(clean.c2ReadCommands, int((150 + cdr::kMaxSectorsPerC2Read - 1) / cdr::kMaxSectorsPerC2Read));
    CHECK(cdr::c2LogLines(b.result) == std::vector<std::string>{"  C2 errors: none"});
}

TEST(c2_transient_error_is_reread) {
    FakeDrive fake = makeC2Disc();
    fake.c2Faults[310] = c2Fault(1000, 16, 1);  // wrong and flagged on the first read only
    const C2Rip r = ripC2(fake, 2, c2Options());
    CHECK(r.bytes == expectedTrackData(300, 150));
    CHECK_EQ(r.result.c2ErrorSectors, 1u);
    CHECK_EQ(r.result.c2Rereads, 1u);
    CHECK_EQ(r.result.c2Recovered, 1u);
    CHECK_EQ(r.result.c2Unresolved, 0u);
    CHECK_EQ(r.result.retries, 0u);
    CHECK(r.result.suspiciousSectors.empty());
    CHECK(r.result.clean());
}

TEST(c2_permanent_error_becomes_suspicious) {
    FakeDrive fake = makeC2Disc();
    fake.c2Faults[310] = c2Fault();
    const int before = fake.c2ReadCommands;
    const C2Rip r = ripC2(fake, 2, c2Options(3));
    CHECK_EQ(fake.c2ReadCommands - before, 7 + 3);  // 7 blocks, 3 re-reads
    CHECK_EQ(r.result.c2ErrorSectors, 1u);
    CHECK_EQ(r.result.c2Rereads, 3u);
    CHECK_EQ(r.result.c2Unresolved, 1u);
    CHECK(r.result.suspiciousSectors == std::vector<uint32_t>{10});
    CHECK(!r.result.clean());
    CHECK(r.result.status() == "1 suspicious sector(s)");
    // Only the flagged bytes differ from the disc (the best read is kept).
    const std::vector<uint8_t> expected = expectedTrackData(300, 150);
    CHECK_EQ(r.bytes.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const size_t inSector = i - size_t(10) * cdr::kSectorBytes;
        const bool flagged = i >= size_t(10) * cdr::kSectorBytes && inSector >= 1000 && inSector < 1016;
        if (!flagged && r.bytes[i] != expected[i]) {
            CHECK(false);
            break;
        }
    }
    const std::vector<std::string> log = cdr::c2LogLines(r.result);
    CHECK_EQ(log.size(), size_t(2));
    CHECK(log[0] == "  C2 errors: 1 sector(s), 3 re-read(s) (0 recovered, 0 identical re-reads with C2, 1 unresolved)");
    CHECK(log[1] == "  Suspicious position 0:00:00 (sector 10)");

    // No retries: suspicious right away.
    FakeDrive none = makeC2Disc();
    none.c2Faults[310] = c2Fault();
    const C2Rip z = ripC2(none, 2, c2Options(0));
    CHECK_EQ(z.result.c2Rereads, 0u);
    CHECK_EQ(z.result.c2Unresolved, 1u);
}

TEST(c2_flagged_but_correct_data_matches) {
    FakeDrive fake = makeC2Disc();
    FakeDrive::C2Fault f = c2Fault();
    f.corrupt = false;  // the drive reports C2 errors for good data
    fake.c2Faults[310] = f;
    const C2Rip r = ripC2(fake, 2, c2Options());
    CHECK(r.bytes == expectedTrackData(300, 150));
    CHECK_EQ(r.result.c2ErrorSectors, 1u);
    CHECK_EQ(r.result.c2Rereads, 1u);  // identical to the read that flagged it
    CHECK_EQ(r.result.c2Matched, 1u);
    CHECK(r.result.clean());

    // The same wrong data on every read ends the same way: only AccurateRip /
    // --verify against another drive can tell.
    FakeDrive same = makeC2Disc();
    FakeDrive::C2Fault g = c2Fault();
    g.varying = false;
    same.c2Faults[310] = g;
    const C2Rip s = ripC2(same, 2, c2Options());
    CHECK_EQ(s.result.c2Matched, 1u);
    CHECK(s.bytes != expectedTrackData(300, 150));
}

TEST(c2_corruption_without_c2_bits_needs_verify_or_accuraterip) {
    FakeDrive fake = makeC2Disc();
    FakeDrive::C2Fault f = c2Fault();
    f.flagged = false;
    f.varying = false;
    fake.c2Faults[310] = f;
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    cdr::Ripper ripper(drive, toc, c2Options());
    cdr::AccurateRipChecksum ar = cdr::AccurateRipChecksum::forTrack(toc, *toc.findTrack(2));
    std::vector<uint8_t> bytes;
    const cdr::TrackRipResult r = ripper.ripTrack(*toc.findTrack(2), [&](const uint8_t* p, size_t n) {
        bytes.insert(bytes.end(), p, p + n);
        ar.update(p, n);
    });
    CHECK(r.clean());  // nothing to see for the ripper
    CHECK_EQ(r.c2ErrorSectors, 0u);
    CHECK(bytes != expectedTrackData(300, 150));
    const std::vector<uint8_t> good = expectedTrackData(300, 150);
    cdr::AccurateRipChecksum ref = cdr::AccurateRipChecksum::forTrack(toc, *toc.findTrack(2));
    ref.update(good.data(), good.size());
    CHECK(ar.v2() != ref.v2());  // AccurateRip would not match

    // Varying silent corruption is caught by --verify (as before C2).
    FakeDrive unstable = makeC2Disc();
    FakeDrive::C2Fault g = c2Fault();
    g.flagged = false;
    unstable.c2Faults[310] = g;
    const C2Rip v = ripC2(unstable, 2, c2Options(2, true));
    CHECK(v.result.retries > 0);
    CHECK_EQ(v.result.unreadableSectors, 1u);
    CHECK_EQ(v.result.c2ErrorSectors, 0u);
}

TEST(c2_verify_mode_needs_two_identical_rereads) {
    FakeDrive fake = makeC2Disc();
    FakeDrive::C2Fault f = c2Fault(1000, 16, 2);
    f.varying = false;  // both block reads return the same wrong data
    fake.c2Faults[310] = f;
    const C2Rip r = ripC2(fake, 2, c2Options(3, true));
    CHECK(r.bytes == expectedTrackData(300, 150));
    CHECK_EQ(r.result.retries, 0u);
    CHECK_EQ(r.result.c2ErrorSectors, 1u);
    CHECK_EQ(r.result.c2Rereads, 2u);  // a clean read, confirmed by a second one
    CHECK_EQ(r.result.c2Recovered, 1u);
    CHECK(r.result.clean());

    FakeDrive flagged = makeC2Disc();
    FakeDrive::C2Fault g = c2Fault();
    g.corrupt = false;
    flagged.c2Faults[310] = g;
    const C2Rip m = ripC2(flagged, 2, c2Options(3, true));
    CHECK_EQ(m.result.c2Rereads, 2u);
    CHECK_EQ(m.result.c2Matched, 1u);
    CHECK(m.bytes == expectedTrackData(300, 150));

    // One re-read is never enough in verify mode.
    FakeDrive once = makeC2Disc();
    FakeDrive::C2Fault h = c2Fault(1000, 16, 2);
    h.varying = false;
    once.c2Faults[310] = h;
    const C2Rip o = ripC2(once, 2, c2Options(1, true));
    CHECK_EQ(o.result.c2Rereads, 1u);
    CHECK_EQ(o.result.c2Unresolved, 1u);
    CHECK(o.result.suspiciousSectors == std::vector<uint32_t>{10});
}

// C2 positions follow the read offset into the output track, also across a
// sector boundary and into the neighbouring track.
TEST(c2_positions_follow_read_offset) {
    {
        FakeDrive fake = makeC2Disc();
        fake.c2Faults[310] = c2Fault(0, 16);  // +30 samples = 120 bytes: end of output sector 9
        CHECK(ripC2(fake, 2, c2Options(1, false, 30)).result.suspiciousSectors == std::vector<uint32_t>{9});
    }
    {
        FakeDrive fake = makeC2Disc();
        fake.c2Faults[310] = c2Fault(100, 30);  // bytes 100..129 straddle output sectors 9 and 10
        CHECK(ripC2(fake, 2, c2Options(1, false, 30)).result.suspiciousSectors == (std::vector<uint32_t>{9, 10}));
    }
    {
        FakeDrive fake = makeC2Disc();
        fake.c2Faults[310] = c2Fault(2200, 100);  // -30 samples: output sectors 10 and 11
        const C2Rip r = ripC2(fake, 2, c2Options(1, false, -30));
        CHECK(r.result.suspiciousSectors == (std::vector<uint32_t>{10, 11}));
    }
    {
        // The first sector of track 2 is read for the end of track 1 at +30.
        FakeDrive fake = makeC2Disc();
        fake.c2Faults[300] = c2Fault(0, 16);
        const C2Rip r1 = ripC2(fake, 1, c2Options(1, false, 30));
        CHECK(r1.result.suspiciousSectors == std::vector<uint32_t>{299});
        CHECK_EQ(r1.result.c2ErrorSectors, 1u);
        // Bytes beyond the output only cost a re-read.
        FakeDrive later = makeC2Disc();
        later.c2Faults[300] = c2Fault(1000, 16);
        const C2Rip r2 = ripC2(later, 1, c2Options(1, false, 30));
        CHECK_EQ(r2.result.c2Unresolved, 1u);
        CHECK(r2.result.suspiciousSectors.empty());
        CHECK(r2.result.clean());
    }
}

TEST(c2_with_read_errors_and_sector_fallback) {
    FakeDrive fake = makeC2Disc();
    fake.failuresBySector[320] = -1;            // unreadable: the block is read sector by sector
    fake.c2Faults[321] = c2Fault(1000, 16, 2);  // flagged on its first two sector reads
    const C2Rip r = ripC2(fake, 2, c2Options(2));
    std::vector<uint8_t> expected = expectedTrackData(300, 150);
    std::memset(expected.data() + size_t(20) * cdr::kSectorBytes, 0, cdr::kSectorBytes);
    CHECK(r.bytes == expected);
    CHECK_EQ(r.result.unreadableSectors, 1u);
    CHECK_EQ(r.result.c2ErrorSectors, 1u);
    CHECK_EQ(r.result.c2Recovered, 1u);
    CHECK(r.result.status() == "1 unreadable sector(s)");

    // A transient read error and a C2 error in the same block.
    FakeDrive both = makeC2Disc();
    both.failuresBySector[305] = 1;
    both.c2Faults[306] = c2Fault(1000, 16, 1);
    const C2Rip b = ripC2(both, 2, c2Options(2));
    CHECK(b.bytes == expectedTrackData(300, 150));
    CHECK_EQ(b.result.retries, 1u);
    CHECK_EQ(b.result.c2ErrorSectors, 1u);
    CHECK(b.result.clean());
}

TEST(c2_rejected_reads_fall_back_to_plain_reads) {
    for (int variant = 0; variant < 2; ++variant) {
        FakeDrive fake = makeC2Disc();
        if (variant == 0) fake.c2ReadsSupported = false;  // ILLEGAL REQUEST
        else fake.c2IgnoresErrorField = true;               // audio only
        fake.c2Faults[310] = c2Fault();
        cdr::CdDrive drive(fake);
        const cdr::Toc toc = drive.readToc();
        cdr::Ripper ripper(drive, toc, c2Options());
        std::vector<uint8_t> bytes;
        const cdr::TrackRipResult r1 = ripper.ripTrack(*toc.findTrack(1), [&](const uint8_t* p, size_t n) {
            bytes.insert(bytes.end(), p, p + n);
        });
        CHECK(bytes == expectedTrackData(0, 300));
        CHECK(r1.c2);
        CHECK(!ripper.c2Active());
        CHECK(r1.c2Fallback.find("READ CD with C2 error pointers failed at LBA 0") == 0);
        CHECK_EQ(r1.retries, 0u);
        CHECK_EQ(fake.c2ReadCommands, 1);  // given up after the first one
        const std::vector<std::string> log = cdr::c2LogLines(r1);
        CHECK(log.size() == 2 && log[1].rfind("  C2 reads given up: READ CD with C2", 0) == 0);

        // The rest of the disc: plain reads, no C2 statistics.
        const cdr::TrackRipResult r2 = ripper.ripTrack(*toc.findTrack(2), [](const uint8_t*, size_t) {});
        CHECK(!r2.c2);
        CHECK(r2.c2Fallback.empty());
        CHECK_EQ(fake.c2ReadCommands, 1);
        CHECK(r2.clean());
    }

    // A failing sector is not mistaken for a drive without C2 support.
    FakeDrive fake = makeC2Disc();
    fake.failuresBySector[5] = 1;
    const C2Rip r = ripC2(fake, 1, c2Options());
    CHECK(r.c2Active);
    CHECK_EQ(r.result.retries, 1u);
}

TEST(c2_is_not_used_for_gap_detection) {
    FakeDrive fake = makeC2Disc();
    fake.pregaps[2] = 30;
    fake.c2Faults[290] = c2Fault();
    cdr::CdDrive drive(fake);
    const cdr::Toc toc = drive.readToc();
    const cdr::DiscGaps gaps = cdr::detectGaps(drive, toc);
    CHECK_EQ(fake.c2ReadCommands, 0);
    CHECK_EQ(gaps.tracks.size(), size_t(3));
}

TEST(suspicious_position_formatting) {
    CHECK(cdr::formatTrackTime(0) == "0:00:00");
    CHECK(cdr::formatTrackTime(75 * 83 + 74) == "0:01:23");
    CHECK(cdr::formatTrackTime(75 * 3600) == "1:00:00");
    CHECK(cdr::suspiciousPositionLines({}).empty());
    const std::vector<std::string> lines = cdr::suspiciousPositionLines({5, 6225, 6226, 6227, 6380});
    CHECK_EQ(lines.size(), size_t(3));
    CHECK(lines[0] == "Suspicious position 0:00:00 (sector 5)");
    CHECK(lines[1] == "Suspicious position 0:01:23 - 0:01:23 (sectors 6225-6227)");
    CHECK(lines[2] == "Suspicious position 0:01:25 (sector 6380)");
    const std::vector<std::string> capped = cdr::suspiciousPositionLines({1, 3, 5, 7}, 2);
    CHECK_EQ(capped.size(), size_t(3));
    CHECK(capped[2] == "... and 2 more suspicious position(s)");

    cdr::TrackRipResult r;
    CHECK(r.status() == "OK");
    r.unreadableSectors = 2;
    r.suspiciousSectors = {1, 2, 3};
    CHECK(r.status() == "2 unreadable sector(s), 3 suspicious sector(s)");
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
