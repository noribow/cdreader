#include "cdreader/mp4.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cdr::mp4 {

namespace {

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) { out.insert(out.end(), data.begin(), data.end()); }

void appendText(std::vector<uint8_t>& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

// Unity transformation matrix of mvhd / tkhd (16.16 and 2.30 fixed point).
void putMatrix(std::vector<uint8_t>& out) {
    const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t v : m) putBe(out, v, 4);
}

// Creation / modification time and duration: 32-bit in version 0, 64-bit in version 1.
uint8_t timeVersion(uint64_t duration) { return duration > std::numeric_limits<uint32_t>::max() ? 1 : 0; }

void putTimes(std::vector<uint8_t>& out, uint8_t version) {
    putBe(out, 0, version ? 8 : 4);  // creation time (0: not given, keeps the output reproducible)
    putBe(out, 0, version ? 8 : 4);  // modification time
}

std::vector<uint8_t> movieHeader(uint32_t timescale, uint64_t duration) {
    const uint8_t version = timeVersion(duration);
    std::vector<uint8_t> p;
    putTimes(p, version);
    putBe(p, timescale, 4);
    putBe(p, duration, version ? 8 : 4);
    putBe(p, 0x00010000, 4);  // rate 1.0
    putBe(p, 0x0100, 2);      // volume 1.0
    p.resize(p.size() + 10);  // reserved
    putMatrix(p);
    p.resize(p.size() + 24);  // pre_defined
    putBe(p, 2, 4);           // next track ID
    return fullBox("mvhd", version, 0, p);
}

std::vector<uint8_t> trackHeader(uint64_t duration) {
    const uint8_t version = timeVersion(duration);
    std::vector<uint8_t> p;
    putTimes(p, version);
    putBe(p, 1, 4);  // track ID
    putBe(p, 0, 4);  // reserved
    putBe(p, duration, version ? 8 : 4);
    p.resize(p.size() + 8);  // reserved
    putBe(p, 0, 2);          // layer
    putBe(p, 0, 2);          // alternate group
    putBe(p, 0x0100, 2);     // volume 1.0 (audio)
    putBe(p, 0, 2);          // reserved
    putMatrix(p);
    putBe(p, 0, 4);  // width
    putBe(p, 0, 4);  // height
    return fullBox("tkhd", version, 0x000003, p);  // enabled, in movie
}

std::vector<uint8_t> mediaHeader(uint32_t timescale, uint64_t duration) {
    const uint8_t version = timeVersion(duration);
    std::vector<uint8_t> p;
    putTimes(p, version);
    putBe(p, timescale, 4);
    putBe(p, duration, version ? 8 : 4);
    putBe(p, 0x55C4, 2);  // language "und" (ISO 639-2/T, 5 bits per letter)
    putBe(p, 0, 2);       // pre_defined
    return fullBox("mdhd", version, 0, p);
}

std::vector<uint8_t> handler(const std::string& type, const std::string& manufacturer, const std::string& name) {
    std::vector<uint8_t> p;
    putBe(p, 0, 4);  // pre_defined
    appendText(p, type);
    appendText(p, manufacturer.empty() ? std::string(4, '\0') : manufacturer);  // reserved[0]
    putBe(p, 0, 8);                                                             // reserved[1..2]
    appendText(p, name);
    p.push_back(0);
    return fullBox("hdlr", 0, 0, p);
}

std::vector<uint8_t> dataInformation() {
    std::vector<uint8_t> dref;
    putBe(dref, 1, 4);                       // entry count
    append(dref, fullBox("url ", 0, 1, {}));  // flag 1: the media data is in this file
    return box("dinf", fullBox("dref", 0, 0, dref));
}

std::vector<uint8_t> sampleTable(const AudioTrack& t) {
    const size_t frames = t.frameSizes.size();
    if (t.frameOffsets.size() != frames) throw std::logic_error("MP4: frame offsets and sizes differ in number");
    if (frames > std::numeric_limits<uint32_t>::max()) throw std::length_error("MP4: too many frames");

    std::vector<uint8_t> stsd;
    putBe(stsd, 1, 4);
    append(stsd, t.sampleEntry);

    // Time to sample: every frame lasts frameDuration, except a shorter last one.
    std::vector<std::pair<uint32_t, uint32_t>> durations;  // (count, duration)
    if (frames > 0) {
        const uint64_t full = uint64_t(frames - 1) * t.frameDuration;
        if (t.duration <= full || t.duration - full > t.frameDuration)
            throw std::logic_error("MP4: duration does not match the frames");
        const uint32_t last = uint32_t(t.duration - full);
        if (frames > 1) durations.emplace_back(uint32_t(frames - 1), t.frameDuration);
        if (!durations.empty() && last == t.frameDuration)
            ++durations.back().first;
        else
            durations.emplace_back(1, last);
    }
    std::vector<uint8_t> stts;
    putBe(stts, durations.size(), 4);
    for (const auto& [count, duration] : durations) {
        putBe(stts, count, 4);
        putBe(stts, duration, 4);
    }

    // Chunks: up to framesPerChunk frames that follow each other in the file.
    std::vector<uint64_t> chunkOffsets;
    std::vector<uint32_t> chunkFrames;
    for (size_t i = 0; i < frames; ++i) {
        const bool contiguous = i > 0 && t.frameOffsets[i] == t.frameOffsets[i - 1] + t.frameSizes[i - 1];
        if (!contiguous || chunkFrames.back() >= t.framesPerChunk) {
            chunkOffsets.push_back(t.frameOffsets[i]);
            chunkFrames.push_back(0);
        }
        ++chunkFrames.back();
    }
    std::vector<std::pair<uint32_t, uint32_t>> runs;  // (first chunk, 1-based; frames per chunk)
    for (size_t c = 0; c < chunkFrames.size(); ++c)
        if (runs.empty() || runs.back().second != chunkFrames[c]) runs.emplace_back(uint32_t(c + 1), chunkFrames[c]);
    std::vector<uint8_t> stsc;
    putBe(stsc, runs.size(), 4);
    for (const auto& [first, count] : runs) {
        putBe(stsc, first, 4);
        putBe(stsc, count, 4);
        putBe(stsc, 1, 4);  // sample description index
    }

    // One size for all frames when they are equal (FFmpeg reads a track whose
    // stts is a single 1-sample entry as old-style uncompressed audio and then
    // needs it), otherwise one entry per frame.
    const bool sameSize =
        frames > 0 && std::all_of(t.frameSizes.begin(), t.frameSizes.end(), [&](uint32_t s) { return s == t.frameSizes[0]; });
    std::vector<uint8_t> stsz;
    putBe(stsz, sameSize ? t.frameSizes[0] : 0, 4);
    putBe(stsz, frames, 4);
    if (!sameSize)
        for (uint32_t size : t.frameSizes) putBe(stsz, size, 4);

    const bool wide = !chunkOffsets.empty() && chunkOffsets.back() > std::numeric_limits<uint32_t>::max();
    std::vector<uint8_t> stco;
    putBe(stco, chunkOffsets.size(), 4);
    for (uint64_t offset : chunkOffsets) putBe(stco, offset, wide ? 8 : 4);

    std::vector<uint8_t> stbl;
    append(stbl, fullBox("stsd", 0, 0, stsd));
    append(stbl, fullBox("stts", 0, 0, stts));
    append(stbl, fullBox("stsc", 0, 0, stsc));
    append(stbl, fullBox("stsz", 0, 0, stsz));
    append(stbl, fullBox(wide ? "co64" : "stco", 0, 0, stco));
    return box("stbl", stbl);
}

// An ilst item holding one "data" box: type 1 = UTF-8 text, 0 = binary.
std::vector<uint8_t> item(const std::string& type, uint32_t dataType, const std::vector<uint8_t>& value) {
    std::vector<uint8_t> data;
    putBe(data, dataType, 4);
    putBe(data, 0, 4);  // locale
    append(data, value);
    return box(type, box("data", data));
}

std::vector<uint8_t> textItem(const std::string& type, const std::string& text) {
    return item(type, 1, std::vector<uint8_t>(text.begin(), text.end()));
}

// "----" item: a reverse-DNS "mean", a "name" and the text value.
std::vector<uint8_t> freeformItem(const std::string& name, const std::string& text) {
    std::vector<uint8_t> p;
    append(p, fullBox("mean", 0, 0, std::vector<uint8_t>{'c', 'o', 'm', '.', 'a', 'p', 'p', 'l', 'e', '.', 'i', 'T',
                                                             'u', 'n', 'e', 's'}));
    append(p, fullBox("name", 0, 0, std::vector<uint8_t>(name.begin(), name.end())));
    std::vector<uint8_t> data;
    putBe(data, 1, 4);  // UTF-8
    putBe(data, 0, 4);  // locale
    appendText(data, text);
    append(p, box("data", data));
    return box("----", p);
}

}  // namespace

void putBe(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

std::vector<uint8_t> box(const std::string& type, const std::vector<uint8_t>& payload) {
    if (type.size() != 4) throw std::logic_error("MP4 box type must have 4 characters");
    if (payload.size() > std::numeric_limits<uint32_t>::max() - 8) throw std::length_error("MP4 box too large");
    std::vector<uint8_t> b;
    b.reserve(payload.size() + 8);
    putBe(b, payload.size() + 8, 4);
    appendText(b, type);
    append(b, payload);
    return b;
}

std::vector<uint8_t> fullBox(const std::string& type, uint8_t version, uint32_t flags,
                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p;
    p.reserve(payload.size() + 4);
    p.push_back(version);
    putBe(p, flags & 0xFFFFFF, 3);
    append(p, payload);
    return box(type, p);
}

std::vector<uint8_t> fileType() {
    std::vector<uint8_t> p;
    appendText(p, "M4A ");  // major brand
    putBe(p, 0, 4);         // minor version
    appendText(p, "M4A mp42isom");
    return box("ftyp", p);
}

std::vector<uint8_t> audioSampleEntry(const std::string& format, unsigned channels, unsigned sampleSize,
                                      uint32_t sampleRate, const std::vector<uint8_t>& children) {
    if (sampleRate > 0xFFFF) throw std::invalid_argument("MP4: sample rate does not fit the sample entry");
    std::vector<uint8_t> p(6, 0);  // reserved
    putBe(p, 1, 2);                // data reference index
    putBe(p, 0, 8);                // reserved (QuickTime: version, revision, vendor)
    putBe(p, channels, 2);
    putBe(p, sampleSize, 2);
    putBe(p, 0, 2);                       // pre_defined (compression ID)
    putBe(p, 0, 2);                       // reserved (packet size)
    putBe(p, uint64_t(sampleRate) << 16, 4);  // 16.16 fixed point
    append(p, children);
    return box(format, p);
}

std::vector<uint8_t> itunesMetadata(const TrackMetadata& m, const std::string& encoder) {
    std::vector<uint8_t> ilst;
    auto text = [&](const char* type, const std::string& value) {
        if (!value.empty()) append(ilst, textItem(type, value));
    };
    text("\xA9nam", m.title);
    text("\xA9" "ART", m.artist);
    text("\xA9" "alb", m.album);
    text("aART", m.albumArtist);
    if (m.trackNumber > 0) {
        std::vector<uint8_t> v;
        putBe(v, 0, 2);
        putBe(v, uint64_t(std::min(m.trackNumber, 0xFFFF)), 2);
        putBe(v, uint64_t(std::max(0, std::min(m.trackTotal, 0xFFFF))), 2);
        putBe(v, 0, 2);
        append(ilst, item("trkn", 0, v));
    }
    text("\xA9" "day", m.year);
    text("\xA9gen", m.genre);
    text("\xA9too", encoder);
    if (!m.discId.empty()) append(ilst, freeformItem("CDDB", m.discId));
    if (!m.isrc.empty()) append(ilst, freeformItem("ISRC", m.isrc));
    if (!m.mcn.empty()) append(ilst, freeformItem("BARCODE", m.mcn));

    std::vector<uint8_t> meta = handler("mdir", "appl", "");
    append(meta, box("ilst", ilst));
    return box("udta", fullBox("meta", 0, 0, meta));
}

std::vector<uint8_t> movie(const AudioTrack& t, const std::vector<uint8_t>& udta) {
    std::vector<uint8_t> minf = fullBox("smhd", 0, 0, std::vector<uint8_t>(4, 0));  // balance, reserved
    append(minf, dataInformation());
    append(minf, sampleTable(t));

    std::vector<uint8_t> mdia = mediaHeader(t.sampleRate, t.duration);
    append(mdia, handler("soun", "", "SoundHandler"));
    append(mdia, box("minf", minf));

    std::vector<uint8_t> trak = trackHeader(t.duration);
    append(trak, box("mdia", mdia));

    std::vector<uint8_t> moov = movieHeader(t.sampleRate, t.duration);
    append(moov, box("trak", trak));
    append(moov, udta);
    return box("moov", moov);
}

}  // namespace cdr::mp4
