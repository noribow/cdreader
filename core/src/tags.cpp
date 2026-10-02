#include "cdreader/tags.h"

#include <stdexcept>
#include <string>

namespace cdr {

namespace {

void appendLe32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

void appendSyncsafe(std::vector<uint8_t>& v, size_t x) {
    if (x >= (size_t(1) << 28)) throw std::length_error("ID3 tag too large");
    for (int shift = 21; shift >= 0; shift -= 7) v.push_back(uint8_t((x >> shift) & 0x7F));
}

void appendText(std::vector<uint8_t>& v, const std::string& s) { v.insert(v.end(), s.begin(), s.end()); }

std::string trackNumberText(const TrackMetadata& m, bool withTotal) {
    if (m.trackNumber <= 0) return {};
    std::string s = std::to_string(m.trackNumber);
    if (withTotal && m.trackTotal > 0) s += "/" + std::to_string(m.trackTotal);
    return s;
}

}  // namespace

// INFO strings are NUL-terminated and each sub-chunk is word aligned; the
// size field counts the terminator but not the pad byte (RIFF rules).
std::vector<uint8_t> riffInfoChunk(const TrackMetadata& m) {
    const std::pair<const char*, std::string> fields[] = {
        {"INAM", m.title},
        {"IART", m.artist},
        {"IPRD", m.album},
        {"ITRK", trackNumberText(m, false)},
        {"ICRD", m.year},
        {"IGNR", m.genre},
        {"ICMT", m.discId.empty() ? std::string() : "CDDB disc ID " + m.discId},
        // No ISRC / MCN here: the INFO id "ISRC" means "source" (who supplied
        // the material), not the recording code, and there is no catalog
        // number id. The WAV writer's id3 chunk carries both (TSRC, BARCODE).
    };
    std::vector<uint8_t> body = {'I', 'N', 'F', 'O'};
    for (const auto& [id, text] : fields) {
        if (text.empty()) continue;
        appendText(body, id);
        appendLe32(body, uint32_t(text.size() + 1));
        appendText(body, text);
        body.push_back(0);
        if (body.size() % 2) body.push_back(0);
    }
    if (body.size() == 4) return {};
    std::vector<uint8_t> chunk = {'L', 'I', 'S', 'T'};
    appendLe32(chunk, uint32_t(body.size()));
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}

std::vector<uint8_t> id3v2Tag(const TrackMetadata& m) {
    std::vector<uint8_t> frames;
    auto frame = [&](const char* id, const std::string& payload) {
        appendText(frames, id);
        appendSyncsafe(frames, payload.size() + 1);  // v2.4 frame sizes are syncsafe
        frames.push_back(0);  // flags
        frames.push_back(0);
        frames.push_back(3);  // text encoding: UTF-8
        appendText(frames, payload);
    };
    auto text = [&](const char* id, const std::string& value) {
        if (!value.empty()) frame(id, value);
    };
    text("TIT2", m.title);
    text("TPE1", m.artist);
    text("TALB", m.album);
    text("TPE2", m.albumArtist);
    text("TRCK", trackNumberText(m, true));
    text("TDRC", m.year);
    text("TCON", m.genre);
    text("TSRC", m.isrc);
    // TXXX: description and value separated by a NUL in the frame's encoding.
    if (!m.discId.empty()) frame("TXXX", std::string("DISCID") + '\0' + m.discId);
    if (!m.mcn.empty()) frame("TXXX", std::string("BARCODE") + '\0' + m.mcn);
    if (frames.empty()) return {};

    std::vector<uint8_t> tag = {'I', 'D', '3', 4, 0, 0};  // version 2.4.0, no flags
    appendSyncsafe(tag, frames.size());
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

std::vector<uint8_t> vorbisComment(const TrackMetadata& m, const std::string& vendor, const std::string& cueSheet) {
    std::vector<std::string> fields;
    auto add = [&](const char* name, const std::string& value) {
        if (!value.empty()) fields.push_back(std::string(name) + "=" + value);
    };
    add("TITLE", m.title);
    add("ARTIST", m.artist);
    add("ALBUM", m.album);
    add("ALBUMARTIST", m.albumArtist);
    if (m.trackNumber > 0) add("TRACKNUMBER", std::to_string(m.trackNumber));
    if (m.trackTotal > 0) add("TRACKTOTAL", std::to_string(m.trackTotal));
    add("DATE", m.year);
    add("GENRE", m.genre);
    add("CDDB", m.discId);
    add("ISRC", m.isrc);
    add("BARCODE", m.mcn);
    add("CUESHEET", cueSheet);

    std::vector<uint8_t> v;
    appendLe32(v, uint32_t(vendor.size()));
    appendText(v, vendor);
    appendLe32(v, uint32_t(fields.size()));
    for (const std::string& f : fields) {
        appendLe32(v, uint32_t(f.size()));
        appendText(v, f);
    }
    return v;
}

}  // namespace cdr
