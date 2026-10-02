// Minimal self-contained test runner (no external dependencies).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/accuraterip.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/metadata.h"
#include "cdreader/crc32.h"
#include "cdreader/http.h"
#include "cdreader/ripper.h"
#include "cdreader/scsi.h"
#include "cdreader/toc.h"
#include "cdreader/wav_writer.h"
#include "fake_drive.h"

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

class FakeHttp : public cdr::HttpClient {
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
    CHECK(r.describe() == "Accurately ripped (confidence 7/13, v1)");

    r = cdr::matchAccurateRip(p, 0, 1, 1u, 2150913158u);
    CHECK_EQ(r.v2Confidence, 6);
    CHECK(r.describe() == "Accurately ripped (confidence 6/13, v2)");

    r = cdr::matchAccurateRip(p, 0, 1, 655774750u, 2150913158u);
    CHECK_EQ(r.confidence(), 13);
    CHECK(r.describe() == "Accurately ripped (confidence 13/13, v1+v2)");

    r = cdr::matchAccurateRip(p, 0, 1, 1u, 2u);
    CHECK(!r.accurate() && r.inDatabase());
    CHECK(r.describe() == "Not accurate (confidence 0/13)");

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
    FakeHttp http;
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
        CHECK_EQ(found[0].confidence, 5);
        CHECK_EQ(found[1].offset, -472);
        CHECK_EQ(found[1].confidence, 2);
    }
    CHECK(cdr::findAccurateRipOffsets(scan, pressings, 0).empty());
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
