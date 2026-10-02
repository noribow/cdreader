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
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/crc32.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/file_name.h"
#include "cdreader/flac_encoder.h"
#include "cdreader/flac_writer.h"
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

TEST(file_names_are_sanitized) {
    CHECK(cdr::safeFileName("AC/DC: Back <in> \"Black\"?", "x") == "AC_DC_ Back _in_ _Black__");
    CHECK(cdr::safeFileName("  Title...  ", "x") == "Title");
    CHECK(cdr::safeFileName(" . ", "fallback") == "fallback");
    CHECK(cdr::safeFileName("con", "x") == "_con");
    CHECK(cdr::safeFileName("LPT1.txt", "x") == "_LPT1.txt");
    CHECK(cdr::safeFileName("COM10", "x") == "COM10");
    const std::string longName = cdr::safeFileName(std::string(149, 'a') + "あいう", "x");
    CHECK(longName == std::string(149, 'a'));  // not cut inside a UTF-8 sequence
    cdr::AlbumMetadata album;
    CHECK(cdr::albumFileBase(album, "CDImage") == "CDImage");
    album.title = "Album";
    CHECK(cdr::albumFileBase(album, "CDImage") == "Album");
    album.artist = "A/B";
    CHECK(cdr::albumFileBase(album, "CDImage") == "A_B - Album");
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
