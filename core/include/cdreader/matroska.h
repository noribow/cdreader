#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

// Matroska (https://www.rfc-editor.org/rfc/rfc9559) / EBML
// (https://www.rfc-editor.org/rfc/rfc8794) writing for audio files (.mka),
// written for the core so that it has no external dependency. Codec
// independent: MkaWriter (mka_writer.h) feeds it the codec's frames.
namespace cdr::mkv {

// Element IDs (written with their marker bits, as in the specification).
namespace id {
constexpr uint32_t kEbml = 0x1A45DFA3, kEbmlVersion = 0x4286, kEbmlReadVersion = 0x42F7,
                   kEbmlMaxIdLength = 0x42F2, kEbmlMaxSizeLength = 0x42F3, kDocType = 0x4282,
                   kDocTypeVersion = 0x4287, kDocTypeReadVersion = 0x4285;
constexpr uint32_t kVoid = 0xEC;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC;
constexpr uint32_t kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1, kDuration = 0x4489, kMuxingApp = 0x4D80,
                   kWritingApp = 0x5741;
constexpr uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackUid = 0x73C5,
                   kTrackType = 0x83, kFlagLacing = 0x9C, kLanguage = 0x22B59C, kCodecId = 0x86,
                   kCodecPrivate = 0x63A2, kCodecDelay = 0x56AA, kSeekPreRoll = 0x56BB,
                   kDefaultDuration = 0x23E383, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F,
                   kBitDepth = 0x6264;
constexpr uint32_t kCluster = 0x1F43B675, kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0,
                   kBlock = 0xA1, kDiscardPadding = 0x75A2;
constexpr uint32_t kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7,
                   kCueTrack = 0xF7, kCueClusterPosition = 0xF1, kCueRelativePosition = 0xF0;
constexpr uint32_t kChapters = 0x1043A770, kEditionEntry = 0x45B9, kEditionUid = 0x45BC, kChapterAtom = 0xB6,
                   kChapterUid = 0x73C4, kChapterTimeStart = 0x91, kChapterTimeEnd = 0x92,
                   kChapterDisplay = 0x80, kChapString = 0x85, kChapLanguage = 0x437C;
constexpr uint32_t kTags = 0x1254C367, kTag = 0x7373, kTargets = 0x63C0, kTargetTypeValue = 0x68CA,
                   kTargetType = 0x63CA, kTagChapterUid = 0x63C4, kSimpleTag = 0x67C8, kTagName = 0x45A3,
                   kTagString = 0x4487;
}  // namespace id

// --- EBML encoding -----------------------------------------------------------

// Bytes of a variable size integer (VINT) for `size` (1..8). 2^n - 1 with all
// value bits set is reserved ("unknown size"), so it takes one byte more.
unsigned sizeLength(uint64_t size);

// Appends `size` as a VINT of `length` bytes (0: the shortest one).
// Throws std::length_error when it does not fit.
void putSize(std::vector<uint8_t>& v, uint64_t size, unsigned length = 0);

// Appends an element ID (1..4 bytes, marker bits included).
void putId(std::vector<uint8_t>& v, uint32_t id);

// Length of an element header (ID + shortest size) for a body of `size` bytes.
size_t headerLength(uint32_t id, uint64_t size);

// Elements. Unsigned integers use the fewest bytes unless `length` is given;
// floats are always 8 bytes (double).
void putUint(std::vector<uint8_t>& v, uint32_t id, uint64_t value, unsigned length = 0);
void putInt(std::vector<uint8_t>& v, uint32_t id, int64_t value);
void putFloat(std::vector<uint8_t>& v, uint32_t id, double value);
void putString(std::vector<uint8_t>& v, uint32_t id, const std::string& value);  // also UTF-8
void putBinary(std::vector<uint8_t>& v, uint32_t id, const std::vector<uint8_t>& value);
void putMaster(std::vector<uint8_t>& v, uint32_t id, const std::vector<uint8_t>& body);

// A Void element of exactly `totalBytes` (at least 2) bytes.
void putVoid(std::vector<uint8_t>& v, size_t totalBytes);

// --- Muxer -------------------------------------------------------------------

constexpr uint64_t kTimestampScaleNs = 1000000;  // block timestamps in milliseconds

struct AudioTrack {
    std::string codecId;                // e.g. "A_FLAC"
    std::vector<uint8_t> codecPrivate;  // empty: none
    double samplingFrequency = 44100;
    unsigned channels = 2;
    unsigned bitDepth = 0;              // 0: not written
    uint64_t codecDelayNs = 0;          // 0: not written
    uint64_t seekPreRollNs = 0;         // 0: not written
    uint64_t defaultDurationNs = 0;     // 0: not written (frames of varying duration)
    uint64_t uid = 1;
};

struct Chapter {
    uint64_t uid = 1;
    uint64_t startNs = 0, endNs = 0;
    std::string title;
};

struct SimpleTag {
    std::string name, value;
};

// A Tag with its Targets: level 50 = ALBUM, 30 = TRACK (RFC 9559 section
// 5.1.8.1.1). With a chapter UID it applies to that chapter only, otherwise
// to the whole file.
struct Tag {
    uint64_t targetTypeValue = 50;
    std::string targetType;  // optional (FFmpeg then prefixes the names with it: "ALBUM/TITLE")
    uint64_t chapterUid = 0;
    std::vector<SimpleTag> simpleTags;
};

// Writes a Matroska file with one audio track to a seekable stream:
//
//   EBML header (DocType "matroska")
//   Segment
//     SeekHead   Info, Tracks, Chapters, Tags and Cues positions
//     Info       TimestampScale (1 ms), MuxingApp / WritingApp, Duration
//     Tracks     the audio track
//     Chapters   (when given)
//     Tags       (when given)
//     Cluster... SimpleBlocks (key frames), at most kClusterMs each
//     Cues       one CuePoint per Cluster
//
// Everything before the Clusters is written by begin(); finish() appends the
// Cues and patches the sizes and positions that are only known at the end
// (Segment size, Duration, the Cues entry of the SeekHead, CodecPrivate
// changed with updateCodecPrivate()), so the file never has elements of
// unknown size.
class Muxer {
public:
    static constexpr int64_t kClusterMs = 5000;           // Cluster duration limit
    static constexpr size_t kClusterBytes = 4 * 1024 * 1024;  // and size limit

    explicit Muxer(std::ostream& out) : out_(out) {}

    void begin(const std::string& writingApp, const AudioTrack& track, const std::vector<Chapter>& chapters,
               const std::vector<Tag>& tags);

    // A frame starting at `timestampNs` (rounded to the timestamp scale;
    // non-decreasing). A non-zero `discardPaddingNs` (end trimming) puts the
    // frame into a BlockGroup with DiscardPadding instead of a SimpleBlock.
    void addFrame(const uint8_t* data, size_t size, uint64_t timestampNs, int64_t discardPaddingNs = 0);

    // Replaces the CodecPrivate written by begin() (same size), e.g. a FLAC
    // STREAMINFO completed at the end. Takes effect in finish().
    void updateCodecPrivate(const std::vector<uint8_t>& codecPrivate);

    // `durationNs`: the length of the audio. Throws std::runtime_error on write errors.
    void finish(double durationNs);

    uint64_t clusters() const { return cuePoints_.size(); }

private:
    struct CuePoint {
        uint64_t timeMs;
        uint64_t clusterPosition;  // relative to the Segment data
    };
    void writeBytes(const std::vector<uint8_t>& bytes);
    void patch(uint64_t position, const std::vector<uint8_t>& bytes);
    void flushCluster();

    std::ostream& out_;
    std::ostream::pos_type base_ = 0;  // stream position of the EBML header
    uint64_t written_ = 0;             // bytes written since base_
    uint64_t segmentSizePos_ = 0;      // file offsets (from base_) of patched values
    uint64_t segmentDataPos_ = 0;
    uint64_t durationPos_ = 0;
    uint64_t cuesSeekPos_ = 0;         // the Cues Seek element (kSeekEntryBytes)
    uint64_t codecPrivatePos_ = 0;
    std::vector<uint8_t> codecPrivate_;
    std::vector<uint8_t> cluster_;     // body of the open Cluster
    int64_t clusterMs_ = -1;           // its timestamp (-1: no open Cluster)
    int64_t lastMs_ = 0;
    std::vector<CuePoint> cuePoints_;
};

}  // namespace cdr::mkv
