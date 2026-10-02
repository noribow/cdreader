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

#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/metadata.h"
#include "cdreader/crc32.h"
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
