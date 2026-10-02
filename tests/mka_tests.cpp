// Unit tests of the Matroska writer (#23): EBML encoding, the element layout
// of small files (SeekHead / Cues / Duration / Tags / Chapters) and the
// codecs inside (FLAC, PCM, and Opus / Vorbis when built in), decoded in
// process. Same minimal runner as test_main.cpp.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cdreader/audio_writer.h"
#include "cdreader/cue_sheet.h"
#include "cdreader/flac_writer.h"
#include "cdreader/matroska.h"
#include "cdreader/mka_writer.h"
#include "cdreader/resampler.h"
#include "cdreader/toc.h"
#ifdef CDREADER_HAVE_OPUS
#include <opus.h>

#include "cdreader/opus_writer.h"
#endif
#ifdef CDREADER_HAVE_VORBIS
#include <vorbis/codec.h>
#endif
#include "ebml_reader.h"
#include "flac_decoder.h"
#include "test_signals.h"
#include "test_temp.h"

namespace fs = std::filesystem;
namespace id = cdr::mkv::id;

namespace {

int failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define TEST(name)                           \
    static void name();                      \
    static Register reg_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                     \
        }                                                                                   \
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

template <typename F>
bool throws(F f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

using Bytes = std::vector<uint8_t>;

Bytes readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path tempFile(const std::string& name) {
    const fs::path dir = cdr_test::testTempDir() / "cdreader_mka_tests";
    fs::create_directories(dir);
    return dir / name;
}

cdr::TrackMetadata trackMetadata() {
    cdr::TrackMetadata m;
    m.trackNumber = 3;
    m.trackTotal = 12;
    m.title = "\xE6\x9B\xB2\xE5\x90\x8D";  // 曲名
    m.artist = "Track Artist";
    m.album = "Album";
    m.albumArtist = "Album Artist";
    m.year = "1999";
    m.genre = "Rock";
    m.discId = "0A0B0C03";
    return m;
}

// --- A parsed file, with the structural checks every file must pass -----------

struct Frame {
    int64_t ms;
    Bytes data;
    bool simpleBlock;
    int64_t discardPaddingNs;
};

struct Parsed {
    Bytes d;
    ebml::Element segment;
    std::vector<ebml::Element> top;  // Segment children
    ebml::Element info, trackEntry;
    double durationMs = 0;
    std::vector<Frame> frames;
    std::vector<ebml::Element> clusters;

    ebml::Element topLevel(uint32_t elementId) const {
        for (const ebml::Element& e : top)
            if (e.id == elementId) return e;
        throw std::runtime_error("missing top-level element");
    }
    bool hasTop(uint32_t elementId) const {
        return std::any_of(top.begin(), top.end(), [&](const ebml::Element& e) { return e.id == elementId; });
    }
};

Parsed parse(Bytes bytes) {
    Parsed p;
    p.d = std::move(bytes);
    const Bytes& d = p.d;
    const std::vector<ebml::Element> file = ebml::children(d, 0, d.size());
    CHECK_EQ(file.size(), size_t(2));  // EBML header, Segment: nothing after the Segment
    CHECK_EQ(file[0].id, id::kEbml);
    CHECK(ebml::stringValue(d, ebml::find(d, file[0], id::kDocType)) == "matroska");
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, file[0], id::kDocTypeVersion)), uint64_t(4));
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, file[0], id::kDocTypeReadVersion)), uint64_t(2));
    p.segment = file[1];
    CHECK_EQ(p.segment.id, id::kSegment);
    CHECK_EQ(p.segment.end(), d.size());
    p.top = ebml::children(d, p.segment);
    const size_t base = p.segment.dataOffset;

    // The SeekHead comes first and points at the top-level elements.
    CHECK_EQ(p.top.at(0).id, id::kSeekHead);
    size_t seekEntries = 0;
    for (const ebml::Element& seek : ebml::all(d, p.top[0], id::kSeek)) {
        const Bytes idBytes = ebml::binaryValue(d, ebml::find(d, seek, id::kSeekId));
        uint32_t target = 0;
        for (uint8_t b : idBytes) target = target << 8 | b;
        const uint64_t position = ebml::uintValue(d, ebml::find(d, seek, id::kSeekPosition));
        const ebml::Element e = ebml::readElement(d, base + size_t(position), p.segment.end());
        CHECK_EQ(e.id, target);
        ++seekEntries;
    }
    size_t indexed = 0;  // top-level elements that should be in the SeekHead
    for (const ebml::Element& e : p.top)
        if (e.id == id::kInfo || e.id == id::kTracks || e.id == id::kChapters || e.id == id::kTags ||
            e.id == id::kCues)
            ++indexed;
    CHECK_EQ(seekEntries, indexed);

    p.info = p.topLevel(id::kInfo);
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.info, id::kTimestampScale)), uint64_t(1000000));
    p.durationMs = ebml::floatValue(d, ebml::find(d, p.info, id::kDuration));
    p.trackEntry = ebml::find(d, p.topLevel(id::kTracks), id::kTrackEntry);
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kTrackNumber)), uint64_t(1));
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kTrackType)), uint64_t(2));
    CHECK(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kTrackUid)) != 0);

    // Clusters and their blocks.
    for (const ebml::Element& c : p.top) {
        if (c.id != id::kCluster) continue;
        p.clusters.push_back(c);
        const int64_t clusterMs = int64_t(ebml::uintValue(d, ebml::find(d, c, id::kTimestamp)));
        for (const ebml::Element& b : ebml::children(d, c)) {
            if (b.id == id::kTimestamp) continue;
            ebml::Element block = b;
            int64_t discard = 0;
            const bool simple = b.id == id::kSimpleBlock;
            if (!simple) {
                CHECK_EQ(b.id, id::kBlockGroup);
                block = ebml::find(d, b, id::kBlock);
                discard = ebml::intValue(d, ebml::find(d, b, id::kDiscardPadding));
            }
            CHECK_EQ(d[block.dataOffset], uint8_t(0x81));  // track 1
            const int16_t rel = int16_t(uint16_t(d[block.dataOffset + 1] << 8 | d[block.dataOffset + 2]));
            const uint8_t flags = d[block.dataOffset + 3];
            CHECK_EQ(flags, uint8_t(simple ? 0x80 : 0x00));  // key frames, no lacing
            p.frames.push_back({clusterMs + rel,
                                Bytes(d.begin() + std::ptrdiff_t(block.dataOffset + 4), d.begin() + std::ptrdiff_t(block.end())),
                                simple, discard});
        }
    }
    for (size_t i = 1; i < p.frames.size(); ++i) CHECK(p.frames[i].ms >= p.frames[i - 1].ms);

    // Cues: one point per Cluster, at its offset and timestamp.
    if (p.clusters.empty()) {
        CHECK(!p.hasTop(id::kCues));
    } else {
        const std::vector<ebml::Element> points = ebml::all(d, p.topLevel(id::kCues), id::kCuePoint);
        CHECK_EQ(points.size(), p.clusters.size());
        for (size_t i = 0; i < points.size() && i < p.clusters.size(); ++i) {
            const ebml::Element pos = ebml::find(d, points[i], id::kCueTrackPositions);
            CHECK_EQ(ebml::uintValue(d, ebml::find(d, pos, id::kCueTrack)), uint64_t(1));
            CHECK_EQ(base + ebml::uintValue(d, ebml::find(d, pos, id::kCueClusterPosition)), p.clusters[i].offset);
            CHECK_EQ(ebml::uintValue(d, ebml::find(d, points[i], id::kCueTime)),
                     ebml::uintValue(d, ebml::find(d, p.clusters[i], id::kTimestamp)));
        }
    }
    return p;
}

Parsed encodeFile(const std::string& format, const Bytes& pcm, const cdr::TrackMetadata& m,
                  const cdr::EmbeddedCueSheet* cue = nullptr, const cdr::EncoderSettings& settings = {}) {
    std::unique_ptr<cdr::AudioWriter> w = cdr::createAudioWriter(format, settings);
    if (!w) throw std::runtime_error("format not available: " + format);
    if (cue) w->setEmbeddedCueSheet(*cue);
    const fs::path path = tempFile("test.mka");
    w->open(path, m);
    for (size_t pos = 0; pos < pcm.size(); pos += 2352 * 7 + 3)
        w->write(pcm.data() + pos, std::min<size_t>(2352 * 7 + 3, pcm.size() - pos));
    w->close();
    Parsed p = parse(readFile(path));
    fs::remove(path);
    return p;
}

// SimpleTag name -> value of the Tag with the given level (and chapter UID).
std::vector<std::pair<std::string, std::string>> tagsOf(const Parsed& p, uint64_t level, uint64_t chapterUid = 0) {
    std::vector<std::pair<std::string, std::string>> v;
    if (!p.hasTop(id::kTags)) return v;
    for (const ebml::Element& tag : ebml::all(p.d, p.topLevel(id::kTags), id::kTag)) {
        const ebml::Element targets = ebml::find(p.d, tag, id::kTargets);
        if (ebml::uintValue(p.d, ebml::find(p.d, targets, id::kTargetTypeValue)) != level) continue;
        const uint64_t uid =
            ebml::has(p.d, targets, id::kTagChapterUid) ? ebml::uintValue(p.d, ebml::find(p.d, targets, id::kTagChapterUid)) : 0;
        if (uid != chapterUid) continue;
        for (const ebml::Element& s : ebml::all(p.d, tag, id::kSimpleTag))
            v.push_back({ebml::stringValue(p.d, ebml::find(p.d, s, id::kTagName)),
                         ebml::stringValue(p.d, ebml::find(p.d, s, id::kTagString))});
    }
    return v;
}

using TagList = std::vector<std::pair<std::string, std::string>>;

// --- EBML encoding -----------------------------------------------------------

TEST(ebml_vint_sizes) {
    auto enc = [](uint64_t size, unsigned length = 0) {
        Bytes v;
        cdr::mkv::putSize(v, size, length);
        return v;
    };
    CHECK(enc(0) == Bytes({0x80}));
    CHECK(enc(1) == Bytes({0x81}));
    CHECK(enc(126) == Bytes({0xFE}));
    CHECK(enc(127) == Bytes({0x40, 0x7F}));  // 0xFF would mean "unknown size"
    CHECK(enc(16382) == Bytes({0x7F, 0xFE}));
    CHECK(enc(16383) == Bytes({0x20, 0x3F, 0xFF}));
    CHECK(enc(0x123456) == Bytes({0x32, 0x34, 0x56}));
    CHECK(enc(0x2345678) == Bytes({0x12, 0x34, 0x56, 0x78}));
    CHECK(enc(0, 8) == Bytes({0x01, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(enc(5, 2) == Bytes({0x40, 0x05}));
    CHECK(enc((uint64_t(1) << 56) - 2) == Bytes({0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}));
    CHECK(throws([&] { enc((uint64_t(1) << 56) - 1); }));
    CHECK(throws([&] { enc(127, 1); }));
    CHECK_EQ(cdr::mkv::sizeLength(126), 1u);
    CHECK_EQ(cdr::mkv::sizeLength(127), 2u);
    CHECK_EQ(cdr::mkv::sizeLength(2097150), 3u);
    CHECK_EQ(cdr::mkv::sizeLength(2097151), 4u);

    // Every value round trips through the independent reader.
    for (uint64_t value : {uint64_t(0), uint64_t(100), uint64_t(127), uint64_t(300), uint64_t(1) << 20,
                           uint64_t(1) << 35, (uint64_t(1) << 49) + 7}) {
        for (unsigned length : {0u, 8u}) {
            const Bytes v = enc(value, length);
            size_t pos = 0;
            CHECK_EQ(ebml::readVint(v, pos, false), value);
            CHECK_EQ(pos, v.size());
        }
    }
}

TEST(ebml_element_encoding) {
    Bytes v;
    cdr::mkv::putId(v, 0xEC);
    cdr::mkv::putId(v, 0x4286);
    cdr::mkv::putId(v, 0x2AD7B1);
    cdr::mkv::putId(v, 0x1A45DFA3);
    CHECK(v == Bytes({0xEC, 0x42, 0x86, 0x2A, 0xD7, 0xB1, 0x1A, 0x45, 0xDF, 0xA3}));

    v.clear();
    cdr::mkv::putUint(v, id::kTrackNumber, 0);
    cdr::mkv::putUint(v, id::kTrackNumber, 0x1234);
    cdr::mkv::putUint(v, id::kSeekPosition, 0x59, 8);
    CHECK(v == Bytes({0xD7, 0x81, 0x00, 0xD7, 0x82, 0x12, 0x34, 0x53, 0xAC, 0x88, 0, 0, 0, 0, 0, 0, 0, 0x59}));

    v.clear();
    cdr::mkv::putInt(v, id::kDiscardPadding, -1);
    cdr::mkv::putInt(v, id::kDiscardPadding, 127);
    cdr::mkv::putInt(v, id::kDiscardPadding, 128);
    cdr::mkv::putInt(v, id::kDiscardPadding, -129);
    CHECK(v == Bytes({0x75, 0xA2, 0x81, 0xFF, 0x75, 0xA2, 0x81, 0x7F, 0x75, 0xA2, 0x82, 0x00, 0x80, 0x75, 0xA2, 0x82,
                      0xFF, 0x7F}));

    v.clear();
    cdr::mkv::putFloat(v, id::kDuration, 1.5);
    CHECK(v == Bytes({0x44, 0x89, 0x88, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0}));

    v.clear();
    cdr::mkv::putString(v, id::kDocType, "matroska");
    CHECK(v == Bytes({0x42, 0x82, 0x88, 'm', 'a', 't', 'r', 'o', 's', 'k', 'a'}));

    // Void elements of any total size >= 2.
    for (size_t total : {size_t(2), size_t(3), size_t(21), size_t(128), size_t(129), size_t(136), size_t(137),
                         size_t(1000)}) {
        v.clear();
        cdr::mkv::putVoid(v, total);
        CHECK_EQ(v.size(), total);
        const ebml::Element e = ebml::readElement(v, 0, v.size());
        CHECK_EQ(e.id, id::kVoid);
        CHECK_EQ(e.end(), total);
    }
    CHECK(throws([&] { cdr::mkv::putVoid(v, 1); }));
}

// --- Muxer layout ------------------------------------------------------------

TEST(muxer_layout_of_a_small_file) {
    std::stringstream out;
    out << "prefix";  // the file need not start at offset 0 of the stream
    cdr::mkv::Muxer muxer(out);
    cdr::mkv::AudioTrack track;
    track.codecId = "A_TEST";
    track.codecPrivate = {1, 2, 3, 4};
    track.uid = 42;
    std::vector<cdr::mkv::Chapter> chapters(2);
    chapters[0] = {7, 0, 1500000000, "One"};
    chapters[1] = {8, 1500000000, 9000000000, "Two"};
    std::vector<cdr::mkv::Tag> tags(1);
    tags[0].simpleTags = {{"TITLE", "Album"}};
    muxer.begin("test app", track, chapters, tags);
    // 13 s of 1 s frames (two Cluster boundaries at 5 s), the last one trimmed.
    for (int i = 0; i < 13; ++i) {
        const uint8_t payload[3] = {uint8_t(i), 0xAA, 0x55};
        muxer.addFrame(payload, sizeof payload, uint64_t(i) * 1000000000u + 400000, i == 12 ? 250000000 : 0);
    }
    muxer.updateCodecPrivate({9, 8, 7, 6});
    CHECK(throws([&] { muxer.updateCodecPrivate({1}); }));
    muxer.finish(12.25e9);

    const std::string s = out.str();
    CHECK(s.compare(0, 6, "prefix") == 0);
    const Parsed p = parse(Bytes(s.begin() + 6, s.end()));
    const Bytes& d = p.d;

    // Element order: SeekHead, Info, Tracks, Chapters, Tags, Clusters, Cues.
    std::vector<uint32_t> order;
    for (const ebml::Element& e : p.top) order.push_back(e.id);
    const std::vector<uint32_t> expected = {id::kSeekHead, id::kInfo,    id::kTracks,  id::kChapters, id::kTags,
                                            id::kCluster,  id::kCluster, id::kCluster, id::kCues};
    CHECK(order == expected);
    CHECK_EQ(ebml::all(d, p.top[0], id::kSeek).size(), size_t(5));
    CHECK(std::abs(p.durationMs - 12250.0) < 1e-9);
    CHECK(ebml::stringValue(d, ebml::find(d, p.info, id::kMuxingApp)) == "test app");
    CHECK(ebml::stringValue(d, ebml::find(d, p.trackEntry, id::kCodecId)) == "A_TEST");
    CHECK(ebml::binaryValue(d, ebml::find(d, p.trackEntry, id::kCodecPrivate)) == Bytes({9, 8, 7, 6}));
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kTrackUid)), uint64_t(42));

    CHECK_EQ(p.frames.size(), size_t(13));
    for (size_t i = 0; i < p.frames.size(); ++i) {
        CHECK_EQ(p.frames[i].ms, int64_t(i) * 1000);  // 0.4 ms rounds down
        CHECK(p.frames[i].data == Bytes({uint8_t(i), 0xAA, 0x55}));
        CHECK_EQ(p.frames[i].simpleBlock, i != 12);
    }
    CHECK_EQ(p.frames[12].discardPaddingNs, int64_t(250000000));
    std::vector<uint64_t> clusterTimes;
    for (const ebml::Element& c : p.clusters) clusterTimes.push_back(ebml::uintValue(d, ebml::find(d, c, id::kTimestamp)));
    CHECK(clusterTimes == std::vector<uint64_t>({0, 5000, 10000}));

    const ebml::Element edition = ebml::find(d, p.topLevel(id::kChapters), id::kEditionEntry);
    const std::vector<ebml::Element> atoms = ebml::all(d, edition, id::kChapterAtom);
    CHECK_EQ(atoms.size(), size_t(2));
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, atoms.at(1), id::kChapterTimeStart)), uint64_t(1500000000));
    CHECK_EQ(ebml::uintValue(d, ebml::find(d, atoms.at(1), id::kChapterTimeEnd)), uint64_t(9000000000));
    CHECK(ebml::stringValue(d, ebml::find(d, ebml::find(d, atoms.at(1), id::kChapterDisplay), id::kChapString)) == "Two");
    CHECK(tagsOf(p, 50) == TagList({{"TITLE", "Album"}}));

    // Going back in time is a programming error.
    std::stringstream out2;
    cdr::mkv::Muxer m2(out2);
    m2.begin("x", track, {}, {});
    const uint8_t b = 0;
    m2.addFrame(&b, 1, 5000000000u);
    CHECK(throws([&] { m2.addFrame(&b, 1, 4000000000u); }));
}

TEST(muxer_without_frames_has_no_cues) {
    std::stringstream out;
    cdr::mkv::Muxer muxer(out);
    cdr::mkv::AudioTrack track;
    track.codecId = "A_PCM/INT/LIT";
    muxer.begin("x", track, {}, {});
    muxer.finish(0);
    const std::string s = out.str();
    const Parsed p = parse(Bytes(s.begin(), s.end()));
    CHECK(!p.hasTop(id::kCues));
    CHECK(!p.hasTop(id::kCluster));
    CHECK_EQ(ebml::all(p.d, p.top[0], id::kSeek).size(), size_t(2));  // Info, Tracks; the Cues entry is a Void
    CHECK_EQ(ebml::all(p.d, p.top[0], id::kVoid).size(), size_t(1));
    CHECK(p.durationMs == 0);
}

TEST(muxer_cluster_size_limit) {
    std::stringstream out;
    cdr::mkv::Muxer muxer(out);
    cdr::mkv::AudioTrack track;
    track.codecId = "A_PCM/INT/LIT";
    muxer.begin("x", track, {}, {});
    const Bytes big(1024 * 1024, 0x11);
    for (int i = 0; i < 9; ++i) muxer.addFrame(big.data(), big.size(), uint64_t(i) * 1000000);  // 1 ms apart
    muxer.finish(9e6);
    const std::string s = out.str();
    const Parsed p = parse(Bytes(s.begin(), s.end()));
    CHECK_EQ(p.frames.size(), size_t(9));
    CHECK_EQ(p.clusters.size(), size_t(3));  // at most 4 MiB per Cluster
}

// --- MkaWriter ---------------------------------------------------------------

TEST(mka_formats_and_settings) {
    const std::vector<std::string> formats = cdr::audioFormats();
    auto listed = [&](const char* f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
    CHECK(listed("mka"));
    CHECK(listed("mka-pcm"));
    CHECK(!listed("mka-flac"));  // an alias of "mka"
    CHECK(cdr::createAudioWriter("mka-flac") != nullptr);
    CHECK(cdr::createAudioWriter("mka")->extension() == "mka");
    CHECK(cdr::createAudioWriter("mka")->canEmbedCueSheet());
    CHECK(!cdr::isLossyFormat("mka"));
    CHECK(!cdr::isLossyFormat("mka-pcm"));
    CHECK(cdr::isLossyFormat("mka-opus"));
    CHECK(cdr::isLossyFormat("mka-vorbis"));
    cdr::EncoderSettings bitrate, quality;
    bitrate.bitrateKbps = 128;
    quality.quality = 4;
    CHECK(throws([&] { cdr::createAudioWriter("mka", bitrate); }));
    CHECK(throws([&] { cdr::createAudioWriter("mka-pcm", quality); }));
    CHECK(cdr::createAudioWriter("mka-xyz") == nullptr);
    CHECK_EQ(listed("mka-opus"), cdr::MkaWriter::codecAvailable(cdr::MkaWriter::Codec::Opus));
    CHECK_EQ(listed("mka-vorbis"), cdr::MkaWriter::codecAvailable(cdr::MkaWriter::Codec::Vorbis));
    if (listed("mka-opus")) {
        CHECK(throws([&] { cdr::createAudioWriter("mka-opus", quality); }));
        cdr::EncoderSettings low;
        low.bitrateKbps = 5;
        CHECK(throws([&] { cdr::createAudioWriter("mka-opus", low); }));
        CHECK(cdr::createAudioWriter("mka-opus", bitrate)->encoderDescription().find("128 kbit/s") != std::string::npos);
    } else {
        CHECK(cdr::createAudioWriter("mka-opus") == nullptr);
        CHECK(throws([] { cdr::MkaWriter w(cdr::MkaWriter::Codec::Opus); }));
    }
    if (listed("mka-vorbis")) {
        cdr::EncoderSettings both = bitrate;
        both.quality = 3;
        CHECK(throws([&] { cdr::createAudioWriter("mka-vorbis", both); }));
        CHECK(cdr::createAudioWriter("mka-vorbis", quality)->encoderDescription().find("in Matroska, VBR quality 4") !=
              std::string::npos);
    } else {
        CHECK(cdr::createAudioWriter("mka-vorbis") == nullptr);
        CHECK(throws([] { cdr::MkaWriter w(cdr::MkaWriter::Codec::Vorbis); }));
    }
}

TEST(mka_pcm_blocks_are_the_input) {
    for (size_t samples : {size_t(0), size_t(1), size_t(4410), size_t(4411), size_t(44100 * 12 + 77)}) {
        const testsig::Signal s = testsig::music(samples);
        const Parsed p = encodeFile("mka-pcm", s.pcm, trackMetadata());
        CHECK(ebml::stringValue(p.d, ebml::find(p.d, p.trackEntry, id::kCodecId)) == "A_PCM/INT/LIT");
        const ebml::Element audio = ebml::find(p.d, p.trackEntry, id::kAudio);
        CHECK_EQ(ebml::uintValue(p.d, ebml::find(p.d, audio, id::kBitDepth)), uint64_t(16));
        CHECK_EQ(ebml::uintValue(p.d, ebml::find(p.d, audio, id::kChannels)), uint64_t(2));
        CHECK(ebml::floatValue(p.d, ebml::find(p.d, audio, id::kSamplingFrequency)) == 44100.0);
        Bytes all;
        for (size_t i = 0; i < p.frames.size(); ++i) {
            CHECK_EQ(p.frames[i].ms, int64_t(i) * 100);  // 100 ms blocks
            CHECK(p.frames[i].simpleBlock);
            all.insert(all.end(), p.frames[i].data.begin(), p.frames[i].data.end());
        }
        CHECK(all == s.pcm);
        CHECK(std::abs(p.durationMs - double(samples) * 1000 / 44100) < 1e-6);
        if (samples > 44100 * 10) CHECK_EQ(p.clusters.size(), size_t(3));
    }
    // A partial sample at the end is an error.
    cdr::MkaWriter w(cdr::MkaWriter::Codec::Pcm);
    const fs::path path = tempFile("partial.mka");
    w.open(path, {});
    const uint8_t three[3] = {1, 2, 3};
    w.write(three, 3);
    CHECK(throws([&] { w.close(); }));
    fs::remove(path);
}

TEST(mka_flac_frames_decode_to_the_input) {
    for (const testsig::Signal& s : testsig::all()) {
        const Parsed p = encodeFile("mka", s.pcm, trackMetadata());
        CHECK(ebml::stringValue(p.d, ebml::find(p.d, p.trackEntry, id::kCodecId)) == "A_FLAC");
        // CodecPrivate + the frames make a native FLAC stream.
        Bytes flac = ebml::binaryValue(p.d, ebml::find(p.d, p.trackEntry, id::kCodecPrivate));
        CHECK_EQ(flac.size(), size_t(42));
        CHECK(Bytes(flac.begin(), flac.begin() + 8) == Bytes({'f', 'L', 'a', 'C', 0x80, 0, 0, 34}));
        const size_t samples = s.pcm.size() / 4;
        for (size_t i = 0; i < p.frames.size(); ++i) {
            const uint64_t first = uint64_t(i) * 4096;
            CHECK_EQ(p.frames[i].ms, int64_t((first * 1000000000u / 44100 + 500000) / 1000000));
            flac.insert(flac.end(), p.frames[i].data.begin(), p.frames[i].data.end());
        }
        CHECK_EQ(p.frames.size(), (samples + 4095) / 4096);
        const DecodedFlac decoded = decodeFlac(flac);  // checks CRCs
        CHECK_EQ(decoded.totalSamples, uint64_t(samples));
        CHECK(decoded.pcm == s.pcm);
        CHECK_EQ(decoded.frames, unsigned(p.frames.size()));
        CHECK(std::abs(p.durationMs - double(samples) * 1000 / 44100) < 1e-6);
        // Same STREAMINFO (with the MD5) as a native FLAC file of the input.
        cdr::FlacWriter native;
        const fs::path path = tempFile("native.flac");
        native.open(path, {});
        native.write(s.pcm.data(), s.pcm.size());
        native.close();
        const Bytes file = readFile(path);
        fs::remove(path);
        CHECK(Bytes(file.begin() + 8, file.begin() + 42) == Bytes(flac.begin() + 8, flac.begin() + 42));
    }
}

TEST(mka_tags_per_track) {
    const testsig::Signal s = testsig::music(1000);
    const Parsed p = encodeFile("mka", s.pcm, trackMetadata());
    CHECK(!p.hasTop(id::kChapters));
    CHECK(tagsOf(p, 50) == TagList({{"TITLE", "Album"},
                                    {"ALBUM", "Album"},
                                    {"ARTIST", "Album Artist"},
                                    {"TOTAL_PARTS", "12"},
                                    {"DATE_RELEASED", "1999"},
                                    {"GENRE", "Rock"},
                                    {"CDDB", "0A0B0C03"}}));
    CHECK(tagsOf(p, 30) ==
          TagList({{"TITLE", "\xE6\x9B\xB2\xE5\x90\x8D"}, {"ARTIST", "Track Artist"}, {"PART_NUMBER", "3"}}));
    // The album tag comes first (FFmpeg merges the levels, the last value wins).
    const std::vector<ebml::Element> tags = ebml::all(p.d, p.topLevel(id::kTags), id::kTag);
    CHECK_EQ(tags.size(), size_t(2));

    // ISRC (track level) and the disc's MCN as BARCODE (album level), when read (#22).
    cdr::TrackMetadata withCodes = trackMetadata();
    withCodes.isrc = "JPXX09912345";
    withCodes.mcn = "4988001234567";
    const Parsed coded = encodeFile("mka", s.pcm, withCodes);
    const TagList codedAlbum = tagsOf(coded, 50);
    CHECK(!codedAlbum.empty() && codedAlbum.back() == std::make_pair(std::string("BARCODE"), std::string("4988001234567")));
    CHECK(tagsOf(coded, 30) == TagList({{"TITLE", "\xE6\x9B\xB2\xE5\x90\x8D"},
                                        {"ARTIST", "Track Artist"},
                                        {"PART_NUMBER", "3"},
                                        {"ISRC", "JPXX09912345"}}));

    // Without metadata there are no Tags at all.
    const Parsed bare = encodeFile("mka", s.pcm, cdr::TrackMetadata{});
    CHECK(!bare.hasTop(id::kTags));
}

TEST(mka_image_chapters_at_cue_positions) {
    // Three tracks of an image; the positions are not whole milliseconds.
    cdr::EmbeddedCueSheet cue;
    const uint32_t starts[] = {0, 301, 452};
    for (int i = 0; i < 3; ++i) {
        cdr::CueTrack t;
        t.number = i + 1;
        t.startSectors = starts[i];
        t.title = i == 1 ? "" : "Song " + std::to_string(i + 1);
        t.performer = "Performer " + std::to_string(i + 1);
        cue.tracks.push_back(t);
    }
    cue.tracks[0].isrc = "JPXX09900001";
    cue.totalSectors = 620;
    cue.mcn = "4988001234567";  // the album metadata has none: taken from the CUE sheet
    cue.text = "REM test\r\n";
    const testsig::Signal s = testsig::music(size_t(cue.totalSectors) * cdr::kSamplesPerSector);
    cdr::TrackMetadata album = trackMetadata();
    album.trackNumber = 0;
    album.title.clear();
    album.artist = album.albumArtist;
    const Parsed p = encodeFile("mka", s.pcm, album, &cue);

    const ebml::Element edition = ebml::find(p.d, p.topLevel(id::kChapters), id::kEditionEntry);
    CHECK(ebml::uintValue(p.d, ebml::find(p.d, edition, id::kEditionUid)) != 0);
    const std::vector<ebml::Element> atoms = ebml::all(p.d, edition, id::kChapterAtom);
    CHECK_EQ(atoms.size(), size_t(3));
    std::vector<uint64_t> uids;
    for (size_t i = 0; i < atoms.size(); ++i) {
        const uint64_t start = ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterTimeStart));
        const uint64_t end = ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterTimeEnd));
        const uint32_t endSectors = i + 1 < atoms.size() ? starts[i + 1] : cue.totalSectors;
        // A CD frame is 1/75 s: exact to the nanosecond after rounding.
        CHECK_EQ(start, uint64_t(std::llround(double(starts[i]) * 1e9 / 75)));
        CHECK_EQ(end, uint64_t(std::llround(double(endSectors) * 1e9 / 75)));
        // ... which is the sample position of the INDEX 01 at 44.1 kHz.
        CHECK_EQ((start * 44100 + 500000000) / 1000000000, uint64_t(starts[i]) * cdr::kSamplesPerSector);
        const ebml::Element display = ebml::find(p.d, atoms[i], id::kChapterDisplay);
        const std::string title = ebml::stringValue(p.d, ebml::find(p.d, display, id::kChapString));
        CHECK(title == (i == 1 ? std::string("Track 02") : "Song " + std::to_string(i + 1)));
        uids.push_back(ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterUid)));
        CHECK(uids.back() != 0);
    }
    CHECK(uids[0] != uids[1] && uids[1] != uids[2] && uids[0] != uids[2]);

    // Album tag, and a tag per chapter instead of the track tag.
    const TagList albumTags = tagsOf(p, 50);
    CHECK(!albumTags.empty() && albumTags[0] == std::make_pair(std::string("TITLE"), std::string("Album")));
    CHECK(tagsOf(p, 30).empty());
    CHECK(albumTags.back() == std::make_pair(std::string("BARCODE"), std::string("4988001234567")));
    CHECK(tagsOf(p, 30, uids[0]) ==
          TagList({{"TITLE", "Song 1"}, {"ARTIST", "Performer 1"}, {"PART_NUMBER", "1"}, {"ISRC", "JPXX09900001"}}));
    CHECK(tagsOf(p, 30, uids[1]) == TagList({{"ARTIST", "Performer 2"}, {"PART_NUMBER", "2"}}));

    // The audio is still the whole image.
    Bytes flac = ebml::binaryValue(p.d, ebml::find(p.d, p.trackEntry, id::kCodecPrivate));
    for (const Frame& f : p.frames) flac.insert(flac.end(), f.data.begin(), f.data.end());
    CHECK(decodeFlac(flac).pcm == s.pcm);
}

// #25: pregaps (INDEX 00) and the HTOA in a disc image. A chapter still
// starts at the track's INDEX 01 and ends at the next INDEX 01 (a pregap is
// the end of the previous chapter, as in the per-track files); an image that
// starts with the HTOA gets a "Hidden Track" chapter before track 1.
TEST(mka_image_chapters_with_pregaps_and_htoa) {
    for (bool htoa : {false, true}) {
        cdr::EmbeddedCueSheet cue;
        const uint32_t starts[] = {htoa ? 40u : 0u, 301, 452};
        for (int i = 0; i < 3; ++i) {
            cdr::CueTrack t;
            t.number = i + 1;
            t.startSectors = starts[i];
            t.title = "Song " + std::to_string(i + 1);
            cue.tracks.push_back(t);
        }
        if (htoa) cue.tracks[0].hasIndex00 = true;  // INDEX 00 at 00:00:00
        cue.tracks[1].hasIndex00 = true;             // a pregap of 11 frames
        cue.tracks[1].index00Sectors = 290;
        cue.tracks[2].laterIndexes = {500};          // INDEX 02: no chapter of its own
        cue.totalSectors = 620;
        cue.text = "REM test\r\n";
        const testsig::Signal s = testsig::music(size_t(cue.totalSectors) * cdr::kSamplesPerSector);
        cdr::TrackMetadata album = trackMetadata();
        album.trackNumber = 0;
        const Parsed p = encodeFile("mka-pcm", s.pcm, album, &cue);

        const ebml::Element edition = ebml::find(p.d, p.topLevel(id::kChapters), id::kEditionEntry);
        const std::vector<ebml::Element> atoms = ebml::all(p.d, edition, id::kChapterAtom);
        std::vector<uint32_t> bounds = {starts[0], starts[1], starts[2], cue.totalSectors};
        std::vector<std::string> titles = {"Song 1", "Song 2", "Song 3"};
        if (htoa) {
            bounds.insert(bounds.begin(), 0);
            titles.insert(titles.begin(), "Hidden Track");
        }
        CHECK_EQ(atoms.size(), titles.size());
        CHECK_EQ(cdr::MkaWriter::hasHtoaChapter(cue), htoa);
        std::vector<uint64_t> uids;
        for (size_t i = 0; i < atoms.size() && i < titles.size(); ++i) {
            const uint64_t start = ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterTimeStart));
            const uint64_t end = ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterTimeEnd));
            CHECK_EQ(start, uint64_t(std::llround(double(bounds[i]) * 1e9 / 75)));
            CHECK_EQ(end, uint64_t(std::llround(double(bounds[i + 1]) * 1e9 / 75)));
            const ebml::Element display = ebml::find(p.d, atoms[i], id::kChapterDisplay);
            CHECK(ebml::stringValue(p.d, ebml::find(p.d, display, id::kChapString)) == titles[i]);
            uids.push_back(ebml::uintValue(p.d, ebml::find(p.d, atoms[i], id::kChapterUid)));
        }
        // Chapter tags follow the chapters: the HTOA is part 0.
        if (htoa && uids.size() == 4) {
            CHECK(tagsOf(p, 30, uids[0]) == TagList({{"TITLE", "Hidden Track"}, {"PART_NUMBER", "0"}}));
            CHECK(uids[0] != uids[1]);
        }
        if (uids.size() >= 3) {
            const uint64_t song2 = uids[uids.size() - 2];
            CHECK(tagsOf(p, 30, song2) == TagList({{"TITLE", "Song 2"}, {"PART_NUMBER", "2"}}));
        }
        // The audio is the whole image, gaps and HTOA included.
        Bytes pcm;
        for (const Frame& f : p.frames) pcm.insert(pcm.end(), f.data.begin(), f.data.end());
        CHECK(pcm == s.pcm);
    }
}

#ifdef CDREADER_HAVE_OPUS
TEST(mka_opus_codec_delay_and_end_trimming) {
    const cdr::Resampler resampler(44100, 48000, 2);
    for (size_t samples : {size_t(0), size_t(1), size_t(1000), size_t(44100 * 6 + 5)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        const Parsed p = encodeFile("mka-opus", s.pcm, trackMetadata());
        const Bytes& d = p.d;
        CHECK(ebml::stringValue(d, ebml::find(d, p.trackEntry, id::kCodecId)) == "A_OPUS");
        const Bytes head = ebml::binaryValue(d, ebml::find(d, p.trackEntry, id::kCodecPrivate));
        CHECK_EQ(head.size(), size_t(19));
        CHECK(std::string(head.begin(), head.begin() + 8) == "OpusHead");
        const uint16_t preSkip = uint16_t(head[10] | head[11] << 8);
        CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kCodecDelay)),
                 uint64_t(preSkip) * 1000000000 / 48000);
        CHECK_EQ(ebml::uintValue(d, ebml::find(d, p.trackEntry, id::kSeekPreRoll)), uint64_t(80000000));
        const ebml::Element audio = ebml::find(d, p.trackEntry, id::kAudio);
        CHECK(ebml::floatValue(d, ebml::find(d, audio, id::kSamplingFrequency)) == 48000.0);

        // Decode as a player does: drop the codec delay, trim the DiscardPadding.
        int error = 0;
        OpusDecoder* dec = opus_decoder_create(48000, 2, &error);
        CHECK(error == OPUS_OK);
        std::vector<float> out;
        float buffer[5760 * 2];
        for (size_t i = 0; i < p.frames.size(); ++i) {
            const Frame& f = p.frames[i];
            CHECK_EQ(f.ms, int64_t(i) * 20);
            CHECK_EQ(f.simpleBlock, i + 1 != p.frames.size());
            const int n = opus_decode_float(dec, f.data.data(), opus_int32(f.data.size()), buffer, 5760, 0);
            CHECK_EQ(n, 960);
            if (n < 0) break;
            size_t keep = size_t(n);
            if (!f.simpleBlock) keep -= size_t(std::llround(double(f.discardPaddingNs) * 48000 / 1e9));
            out.insert(out.end(), buffer, buffer + keep * 2);
        }
        opus_decoder_destroy(dec);
        const size_t expected = size_t(resampler.outputLength(samples));
        CHECK_EQ(out.size() / 2, preSkip + expected);
        CHECK(std::abs(p.durationMs - double(expected) * 1000 / 48000) < 1e-6);
        if (samples > 44100) {
            double signal = 0, noise = 0;
            for (size_t i = 2400; i + 2400 < expected; ++i) {
                for (int c = 0; c < 2; ++c) {
                    const double ref = testsig::tones(double(i) / 48000, c);
                    const double got = out[(preSkip + i) * 2 + size_t(c)];
                    signal += ref * ref;
                    noise += (got - ref) * (got - ref);
                }
            }
            const double snr = 10 * std::log10(signal / noise);
            std::printf("    opus in mka: SNR %.1f dB\n", snr);
            CHECK(snr > 20);
        }
    }
}
#endif

#ifdef CDREADER_HAVE_VORBIS
TEST(mka_vorbis_headers_and_end_trimming) {
    for (size_t samples : {size_t(1), size_t(1000), size_t(44100 * 3 + 5)}) {
        const testsig::Signal s = testsig::tonesPcm(samples);
        const Parsed p = encodeFile("mka-vorbis", s.pcm, trackMetadata());
        const Bytes& d = p.d;
        CHECK(ebml::stringValue(d, ebml::find(d, p.trackEntry, id::kCodecId)) == "A_VORBIS");
        // Xiph lacing of the three headers.
        const Bytes priv = ebml::binaryValue(d, ebml::find(d, p.trackEntry, id::kCodecPrivate));
        CHECK_EQ(priv.at(0), uint8_t(2));
        size_t pos = 1;
        size_t sizes[3] = {0, 0, 0};
        for (int k = 0; k < 2; ++k) {
            while (priv.at(pos) == 255) sizes[k] += priv[pos++];
            sizes[k] += priv[pos++];
        }
        sizes[2] = priv.size() - pos - sizes[0] - sizes[1];
        std::vector<Bytes> headers;
        for (size_t size : sizes) {
            headers.emplace_back(priv.begin() + std::ptrdiff_t(pos), priv.begin() + std::ptrdiff_t(pos + size));
            pos += size;
        }
        CHECK_EQ(headers[0].at(0), uint8_t(1));
        CHECK_EQ(headers[1].at(0), uint8_t(3));
        CHECK_EQ(headers[2].at(0), uint8_t(5));
        CHECK(std::string(headers[0].begin() + 1, headers[0].begin() + 7) == "vorbis");

        vorbis_info info;
        vorbis_comment comment;
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
        for (size_t k = 0; k < 3; ++k) {
            ogg_packet op{};
            op.packet = headers[k].data();
            op.bytes = long(headers[k].size());
            op.b_o_s = k == 0;
            CHECK_EQ(vorbis_synthesis_headerin(&info, &comment, &op), 0);
        }
        CHECK_EQ(comment.comments, 0);  // the tags are Matroska Tags
        vorbis_dsp_state dsp;
        vorbis_block block;
        CHECK_EQ(vorbis_synthesis_init(&dsp, &info), 0);
        vorbis_block_init(&dsp, &block);
        size_t total = 0;
        std::vector<float> left;
        for (size_t i = 0; i < p.frames.size(); ++i) {
            const Frame& f = p.frames[i];
            ogg_packet op{};
            op.packet = const_cast<uint8_t*>(f.data.data());
            op.bytes = long(f.data.size());
            op.packetno = int64_t(i) + 3;
            CHECK_EQ(vorbis_synthesis(&block, &op), 0);
            vorbis_synthesis_blockin(&dsp, &block);
            float** pcm;
            int n;
            size_t produced = 0;
            const size_t before = total;
            while ((n = vorbis_synthesis_pcmout(&dsp, &pcm)) > 0) {
                left.insert(left.end(), pcm[0], pcm[0] + n);
                produced += size_t(n);
                vorbis_synthesis_read(&dsp, n);
            }
            // Each block starts where the previous output ended.
            CHECK(std::llabs(f.ms - std::llround(double(before) * 1000 / 44100)) <= 1);
            total += produced;
            if (!f.simpleBlock) total -= size_t(std::llround(double(f.discardPaddingNs) * 44100 / 1e9));
        }
        vorbis_block_clear(&block);
        vorbis_dsp_clear(&dsp);
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
        CHECK_EQ(total, samples);
        CHECK(!p.frames.empty() && !p.frames.back().simpleBlock);  // the end is trimmed
        CHECK(std::abs(p.durationMs - double(samples) * 1000 / 44100) < 1e-6);
    }
}
#endif

}  // namespace

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
    fs::remove_all(cdr_test::testTempDir() / "cdreader_mka_tests");
    std::printf("\n%s (%zu tests)\n", failures ? "FAILED" : "PASSED", registry().size());
    return failures ? 1 : 0;
}
