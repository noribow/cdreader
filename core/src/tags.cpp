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
    // TXXX: description and value separated by a NUL in the frame's encoding.
    if (!m.discId.empty()) frame("TXXX", std::string("DISCID") + '\0' + m.discId);
    if (frames.empty()) return {};

    std::vector<uint8_t> tag = {'I', 'D', '3', 4, 0, 0};  // version 2.4.0, no flags
    appendSyncsafe(tag, frames.size());
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

}  // namespace cdr
