#include "cdreader/matroska.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace cdr::mkv {

namespace {

constexpr unsigned kPositionBytes = 8;  // SeekPosition values, so that they can be patched in place
// Seek { SeekID (4-byte ID), SeekPosition (8 bytes) }
constexpr size_t kSeekEntryBytes = 3 + (3 + 4) + (3 + kPositionBytes);

void putBigEndian(std::vector<uint8_t>& v, uint64_t x, unsigned bytes) {
    for (unsigned i = bytes; i-- > 0;) v.push_back(uint8_t(x >> (8 * i)));
}

unsigned uintLength(uint64_t value) {
    unsigned n = 1;
    while (n < 8 && (value >> (8 * n)) != 0) ++n;
    return n;
}

std::vector<uint8_t> seekEntry(uint32_t elementId, uint64_t position) {
    std::vector<uint8_t> body;
    std::vector<uint8_t> idBytes;
    putId(idBytes, elementId);
    putBinary(body, id::kSeekId, idBytes);
    putUint(body, id::kSeekPosition, position, kPositionBytes);
    std::vector<uint8_t> v;
    putMaster(v, id::kSeek, body);
    return v;
}

}  // namespace

unsigned sizeLength(uint64_t size) {
    unsigned n = 1;
    // n bytes hold 7n value bits; all ones is reserved.
    while (n < 8 && size >= (uint64_t(1) << (7 * n)) - 1) ++n;
    return n;
}

void putSize(std::vector<uint8_t>& v, uint64_t size, unsigned length) {
    if (length == 0) length = sizeLength(size);
    if (length > 8 || size >= (uint64_t(1) << (7 * length)) - 1) throw std::length_error("EBML size too large");
    const uint64_t marked = size | uint64_t(1) << (7 * length);
    putBigEndian(v, marked, length);
}

void putId(std::vector<uint8_t>& v, uint32_t elementId) {
    putBigEndian(v, elementId, uintLength(elementId));
}

size_t headerLength(uint32_t elementId, uint64_t size) { return uintLength(elementId) + sizeLength(size); }

void putUint(std::vector<uint8_t>& v, uint32_t elementId, uint64_t value, unsigned length) {
    if (length == 0) length = uintLength(value);
    putId(v, elementId);
    putSize(v, length);
    putBigEndian(v, value, length);
}

void putInt(std::vector<uint8_t>& v, uint32_t elementId, int64_t value) {
    unsigned length = 1;
    while (length < 8) {
        const int64_t limit = int64_t(1) << (8 * length - 1);
        if (value >= -limit && value < limit) break;
        ++length;
    }
    putId(v, elementId);
    putSize(v, length);
    putBigEndian(v, uint64_t(value), length);
}

void putFloat(std::vector<uint8_t>& v, uint32_t elementId, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    putId(v, elementId);
    putSize(v, 8);
    putBigEndian(v, bits, 8);
}

void putString(std::vector<uint8_t>& v, uint32_t elementId, const std::string& value) {
    putId(v, elementId);
    putSize(v, value.size());
    v.insert(v.end(), value.begin(), value.end());
}

void putBinary(std::vector<uint8_t>& v, uint32_t elementId, const std::vector<uint8_t>& value) {
    putId(v, elementId);
    putSize(v, value.size());
    v.insert(v.end(), value.begin(), value.end());
}

void putMaster(std::vector<uint8_t>& v, uint32_t elementId, const std::vector<uint8_t>& body) {
    putBinary(v, elementId, body);
}

void putVoid(std::vector<uint8_t>& v, size_t totalBytes) {
    if (totalBytes < 2) throw std::invalid_argument("a Void element takes at least 2 bytes");
    putId(v, id::kVoid);
    // A one-byte size up to 126 bytes of payload, otherwise an 8-byte size.
    if (totalBytes - 2 < 127) {
        putSize(v, totalBytes - 2, 1);
        v.resize(v.size() + totalBytes - 2);
    } else {
        putSize(v, totalBytes - 9, 8);
        v.resize(v.size() + totalBytes - 9);
    }
}

// --- Muxer -------------------------------------------------------------------

void Muxer::writeBytes(const std::vector<uint8_t>& bytes) {
    out_.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out_) throw std::runtime_error("write error while saving Matroska data");
    written_ += bytes.size();
}

void Muxer::patch(uint64_t position, const std::vector<uint8_t>& bytes) {
    out_.seekp(base_ + std::streamoff(position));
    out_.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out_) throw std::runtime_error("write error while finalizing Matroska data");
}

void Muxer::begin(const std::string& writingApp, const AudioTrack& track, const std::vector<Chapter>& chapters,
                  const std::vector<Tag>& tags) {
    base_ = out_.tellp();
    if (base_ == std::ostream::pos_type(-1)) throw std::runtime_error("Matroska output must be seekable");
    written_ = 0;
    cluster_.clear();
    clusterMs_ = -1;
    lastMs_ = 0;
    cuePoints_.clear();

    // EBML header. DocTypeVersion 4 for CodecDelay / SeekPreRoll / DiscardPadding,
    // read version 2 for SimpleBlock.
    std::vector<uint8_t> body;
    putUint(body, id::kEbmlVersion, 1);
    putUint(body, id::kEbmlReadVersion, 1);
    putUint(body, id::kEbmlMaxIdLength, 4);
    putUint(body, id::kEbmlMaxSizeLength, 8);
    putString(body, id::kDocType, "matroska");
    putUint(body, id::kDocTypeVersion, 4);
    putUint(body, id::kDocTypeReadVersion, 2);
    std::vector<uint8_t> head;
    putMaster(head, id::kEbml, body);

    // Segment header with an 8-byte size, patched in finish().
    putId(head, id::kSegment);
    segmentSizePos_ = head.size();
    putSize(head, 0, 8);
    segmentDataPos_ = head.size();

    // Info, with the Duration value last so that its position is easy to find.
    std::vector<uint8_t> info;
    {
        std::vector<uint8_t> b;
        putUint(b, id::kTimestampScale, kTimestampScaleNs);
        putString(b, id::kMuxingApp, writingApp);
        putString(b, id::kWritingApp, writingApp);
        putFloat(b, id::kDuration, 0);
        putMaster(info, id::kInfo, b);
    }

    // Tracks
    std::vector<uint8_t> tracks;
    size_t codecPrivateInTracks = 0;
    {
        std::vector<uint8_t> audio;
        putFloat(audio, id::kSamplingFrequency, track.samplingFrequency);
        putUint(audio, id::kChannels, track.channels);
        if (track.bitDepth) putUint(audio, id::kBitDepth, track.bitDepth);

        std::vector<uint8_t> entry;
        putUint(entry, id::kTrackNumber, 1);
        putUint(entry, id::kTrackUid, track.uid);
        putUint(entry, id::kTrackType, 2);  // audio
        putUint(entry, id::kFlagLacing, 0);
        putString(entry, id::kLanguage, "und");
        putString(entry, id::kCodecId, track.codecId);
        if (track.defaultDurationNs) putUint(entry, id::kDefaultDuration, track.defaultDurationNs);
        if (track.codecDelayNs) putUint(entry, id::kCodecDelay, track.codecDelayNs);
        if (track.seekPreRollNs) putUint(entry, id::kSeekPreRoll, track.seekPreRollNs);
        putMaster(entry, id::kAudio, audio);
        size_t codecPrivateInEntry = 0;
        if (!track.codecPrivate.empty()) {
            putBinary(entry, id::kCodecPrivate, track.codecPrivate);
            codecPrivateInEntry = entry.size() - track.codecPrivate.size();
        }
        std::vector<uint8_t> tracksBody;
        putMaster(tracksBody, id::kTrackEntry, entry);
        const size_t entryHeader = tracksBody.size() - entry.size();
        putMaster(tracks, id::kTracks, tracksBody);
        if (codecPrivateInEntry)
            codecPrivateInTracks = (tracks.size() - tracksBody.size()) + entryHeader + codecPrivateInEntry;
    }
    codecPrivate_ = track.codecPrivate;

    // Chapters: one edition, a chapter per track.
    std::vector<uint8_t> chapterBytes;
    if (!chapters.empty()) {
        std::vector<uint8_t> edition;
        const uint64_t editionUid = chapters.front().uid ^ 0x45444954u;  // "EDIT"
        putUint(edition, id::kEditionUid, editionUid ? editionUid : 1);
        for (const Chapter& c : chapters) {
            std::vector<uint8_t> display;
            putString(display, id::kChapString, c.title);
            putString(display, id::kChapLanguage, "und");
            std::vector<uint8_t> atom;
            putUint(atom, id::kChapterUid, c.uid);
            putUint(atom, id::kChapterTimeStart, c.startNs);
            putUint(atom, id::kChapterTimeEnd, c.endNs);
            putMaster(atom, id::kChapterDisplay, display);
            putMaster(edition, id::kChapterAtom, atom);
        }
        std::vector<uint8_t> b;
        putMaster(b, id::kEditionEntry, edition);
        putMaster(chapterBytes, id::kChapters, b);
    }

    // Tags
    std::vector<uint8_t> tagBytes;
    if (!tags.empty()) {
        std::vector<uint8_t> b;
        for (const Tag& t : tags) {
            std::vector<uint8_t> targets;
            putUint(targets, id::kTargetTypeValue, t.targetTypeValue);
            if (!t.targetType.empty()) putString(targets, id::kTargetType, t.targetType);
            if (t.chapterUid) putUint(targets, id::kTagChapterUid, t.chapterUid);
            std::vector<uint8_t> tag;
            putMaster(tag, id::kTargets, targets);
            for (const SimpleTag& s : t.simpleTags) {
                std::vector<uint8_t> simple;
                putString(simple, id::kTagName, s.name);
                putString(simple, id::kTagString, s.value);
                putMaster(tag, id::kSimpleTag, simple);
            }
            putMaster(b, id::kTag, tag);
        }
        putMaster(tagBytes, id::kTags, b);
    }

    // SeekHead: its size only depends on the number of entries.
    const size_t entries = 3 + !chapterBytes.empty() + !tagBytes.empty();  // Info, Tracks, Cues (+ Chapters, Tags)
    const size_t seekHeadBytes = headerLength(id::kSeekHead, entries * kSeekEntryBytes) + entries * kSeekEntryBytes;
    uint64_t position = seekHeadBytes;  // relative to the Segment data
    std::vector<uint8_t> seekBody;
    auto add = [&](uint32_t elementId, const std::vector<uint8_t>& element) {
        if (element.empty()) return;
        const std::vector<uint8_t> e = seekEntry(elementId, position);
        seekBody.insert(seekBody.end(), e.begin(), e.end());
        position += element.size();
    };
    add(id::kInfo, info);
    add(id::kTracks, tracks);
    add(id::kChapters, chapterBytes);
    add(id::kTags, tagBytes);
    const size_t cuesSeekInBody = seekBody.size();
    const std::vector<uint8_t> cuesEntry = seekEntry(id::kCues, 0);  // patched in finish()
    seekBody.insert(seekBody.end(), cuesEntry.begin(), cuesEntry.end());
    std::vector<uint8_t> seekHead;
    putMaster(seekHead, id::kSeekHead, seekBody);
    if (seekHead.size() != seekHeadBytes) throw std::logic_error("Matroska SeekHead size");

    cuesSeekPos_ = segmentDataPos_ + (seekHead.size() - seekBody.size()) + cuesSeekInBody;
    durationPos_ = segmentDataPos_ + seekHead.size() + info.size() - 8;
    codecPrivatePos_ =
        codecPrivateInTracks ? segmentDataPos_ + seekHead.size() + info.size() + codecPrivateInTracks : 0;

    writeBytes(head);
    writeBytes(seekHead);
    writeBytes(info);
    writeBytes(tracks);
    writeBytes(chapterBytes);
    writeBytes(tagBytes);
}

void Muxer::addFrame(const uint8_t* data, size_t size, uint64_t timestampNs, int64_t discardPaddingNs) {
    const int64_t ms = int64_t((timestampNs + kTimestampScaleNs / 2) / kTimestampScaleNs);
    if (ms < lastMs_) throw std::logic_error("Matroska frames must not go back in time");
    lastMs_ = ms;
    if (clusterMs_ >= 0 && (ms - clusterMs_ >= kClusterMs || cluster_.size() + size > kClusterBytes)) flushCluster();
    if (clusterMs_ < 0) {
        clusterMs_ = ms;
        cluster_.clear();
        putUint(cluster_, id::kTimestamp, uint64_t(ms));
    }
    // Block header: track number 1 (VINT), timestamp relative to the Cluster
    // (16-bit signed), flags.
    const int16_t relative = int16_t(ms - clusterMs_);
    std::vector<uint8_t> block = {0x81, uint8_t(uint16_t(relative) >> 8), uint8_t(relative)};
    if (discardPaddingNs == 0) {
        block.push_back(0x80);  // key frame
        block.insert(block.end(), data, data + size);
        putBinary(cluster_, id::kSimpleBlock, block);
    } else {
        block.push_back(0x00);  // a Block has no key frame flag (no ReferenceBlock: key frame)
        block.insert(block.end(), data, data + size);
        std::vector<uint8_t> group;
        putBinary(group, id::kBlock, block);
        putInt(group, id::kDiscardPadding, discardPaddingNs);
        putMaster(cluster_, id::kBlockGroup, group);
    }
}

void Muxer::flushCluster() {
    if (clusterMs_ < 0) return;
    cuePoints_.push_back({uint64_t(clusterMs_), written_ - segmentDataPos_});
    std::vector<uint8_t> c;
    putMaster(c, id::kCluster, cluster_);
    writeBytes(c);
    cluster_.clear();
    clusterMs_ = -1;
}

void Muxer::updateCodecPrivate(const std::vector<uint8_t>& codecPrivate) {
    if (codecPrivate.size() != codecPrivate_.size()) throw std::logic_error("CodecPrivate size changed");
    codecPrivate_ = codecPrivate;
}

void Muxer::finish(double durationNs) {
    flushCluster();
    const uint64_t cuesPos = written_ - segmentDataPos_;
    if (!cuePoints_.empty()) {
        std::vector<uint8_t> body;
        for (const CuePoint& p : cuePoints_) {
            std::vector<uint8_t> positions;
            putUint(positions, id::kCueTrack, 1);
            putUint(positions, id::kCueClusterPosition, p.clusterPosition);
            std::vector<uint8_t> point;
            putUint(point, id::kCueTime, p.timeMs);
            putMaster(point, id::kCueTrackPositions, positions);
            putMaster(body, id::kCuePoint, point);
        }
        std::vector<uint8_t> cues;
        putMaster(cues, id::kCues, body);
        writeBytes(cues);
    }
    const uint64_t end = written_;

    std::vector<uint8_t> v;
    putSize(v, end - segmentDataPos_, 8);
    patch(segmentSizePos_, v);
    v.clear();
    putBigEndian(v, [&] {
        const double ms = std::isfinite(durationNs) && durationNs > 0 ? durationNs / double(kTimestampScaleNs) : 0.0;
        uint64_t bits;
        std::memcpy(&bits, &ms, sizeof bits);
        return bits;
    }(), 8);
    patch(durationPos_, v);
    // Without any Cluster (no audio) there are no Cues: the entry becomes a Void.
    v.clear();
    if (cuePoints_.empty()) putVoid(v, kSeekEntryBytes);
    else v = seekEntry(id::kCues, cuesPos);
    patch(cuesSeekPos_, v);
    if (codecPrivatePos_) patch(codecPrivatePos_, codecPrivate_);
    out_.seekp(base_ + std::streamoff(end));
    out_.flush();
    if (!out_) throw std::runtime_error("write error while finalizing Matroska data");
}

}  // namespace cdr::mkv
