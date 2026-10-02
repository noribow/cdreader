#include "cdreader/flac_writer.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr unsigned kBytesPerFrame = 4;  // one 16-bit stereo sample
constexpr size_t kBlockBytes = size_t(flac::kBlockSize) * kBytesPerFrame;

// SEEKTABLE + PADDING share a fixed area reserved when the file is opened,
// because the number of seek points is only known at the end.
constexpr uint32_t kReservedBytes = 8192;
constexpr uint32_t kMaxSeekPoints = 100;
constexpr uint64_t kSeekInterval = 10 * kSampleRate;  // a seek point every 10 s

enum BlockType : uint8_t { kStreamInfo = 0, kPadding = 1, kSeekTable = 3, kVorbisComment = 4, kCueSheet = 5 };

// The CUESHEET tag holds plain UTF-8 text; the .cue file's BOM does not belong there.
std::string withoutBom(const std::string& text) {
    return text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text;
}

void putBlockHeader(std::vector<uint8_t>& v, BlockType type, bool last, uint32_t length) {
    v.push_back(uint8_t((last ? 0x80 : 0) | type));
    v.push_back(uint8_t(length >> 16));
    v.push_back(uint8_t(length >> 8));
    v.push_back(uint8_t(length));
}

void putBigEndian(std::vector<uint8_t>& v, uint64_t x, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

FlacWriter::FlacWriter(flac::EncoderOptions options) : encoder_(options) {}

FlacWriter::~FlacWriter() {
    try {
        close();
    } catch (...) {
    }
}

void FlacWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    pending_.clear();
    frameOffsets_.clear();
    md5_ = Md5();
    totalSamples_ = audioBytes_ = 0;
    minFrameBytes_ = maxFrameBytes_ = 0;

    std::vector<uint8_t> head = {'f', 'L', 'a', 'C'};
    putBlockHeader(head, kStreamInfo, false, 34);
    head.resize(head.size() + 34);  // written in close()
    const std::vector<uint8_t> tags =
        flac::vorbisComment(metadata, "cdreader " CDREADER_VERSION, cue_ ? withoutBom(cue_->text) : std::string());
    if (tags.size() >= (1u << 24)) throw std::runtime_error("FLAC tags too large");
    putBlockHeader(head, kVorbisComment, false, uint32_t(tags.size()));
    head.insert(head.end(), tags.begin(), tags.end());
    leadOutOffsetPos_ = 0;
    if (cue_) {
        const std::vector<uint8_t> sheet =
            flac::cueSheet(*cue_, uint64_t(cue_->totalSectors) * kSamplesPerSector);
        putBlockHeader(head, kCueSheet, false, uint32_t(sheet.size()));
        leadOutOffsetPos_ = head.size() + flac::cueSheetLeadOutOffsetPosition(*cue_);
        head.insert(head.end(), sheet.begin(), sheet.end());
    }
    reservedOffset_ = head.size();
    putBlockHeader(head, kPadding, true, kReservedBytes - 4);
    head.resize(head.size() + kReservedBytes - 4);
    writeBytes(head.data(), head.size());
}

void FlacWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open()) throw std::runtime_error("FLAC file is not open");
    size_t used = 0;
    if (!pending_.empty()) {
        used = std::min(bytes, kBlockBytes - pending_.size());
        pending_.insert(pending_.end(), pcm, pcm + used);
        if (pending_.size() < kBlockBytes) return;
        encodeBlock(pending_.data(), flac::kBlockSize);
        pending_.clear();
    }
    for (; bytes - used >= kBlockBytes; used += kBlockBytes) encodeBlock(pcm + used, flac::kBlockSize);
    pending_.insert(pending_.end(), pcm + used, pcm + bytes);
}

void FlacWriter::encodeBlock(const uint8_t* pcm, unsigned samples) {
    md5_.update(pcm, size_t(samples) * kBytesPerFrame);  // FLAC hashes the little-endian PCM
    left_.resize(samples);
    right_.resize(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const uint8_t* p = pcm + size_t(i) * kBytesPerFrame;
        left_[i] = int16_t(uint16_t(p[0] | p[1] << 8));
        right_[i] = int16_t(uint16_t(p[2] | p[3] << 8));
    }
    const std::vector<uint8_t> frame =
        encoder_.encode(left_.data(), right_.data(), samples, uint32_t(frameOffsets_.size()));
    frameOffsets_.push_back(audioBytes_);
    writeBytes(frame.data(), frame.size());
    const uint32_t size = uint32_t(frame.size());
    minFrameBytes_ = frameOffsets_.size() == 1 ? size : std::min(minFrameBytes_, size);
    maxFrameBytes_ = std::max(maxFrameBytes_, size);
    audioBytes_ += size;
    totalSamples_ += samples;
}

void FlacWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving FLAC data");
}

void FlacWriter::close() {
    if (!out_.is_open()) return;
    if (pending_.size() % kBytesPerFrame) {
        out_.close();
        throw std::runtime_error("FLAC input ends in the middle of a sample");
    }
    if (!pending_.empty()) encodeBlock(pending_.data(), unsigned(pending_.size() / kBytesPerFrame));
    pending_.clear();
    if (totalSamples_ >= (uint64_t(1) << 36)) throw std::runtime_error("FLAC stream too long");

    // STREAMINFO. The block size fields describe every block but the last one;
    // a stream of a single short block reports that block's size (at least 16).
    const uint64_t blockSize = totalSamples_ > flac::kBlockSize
                                   ? flac::kBlockSize
                                   : std::max<uint64_t>(totalSamples_, 16);
    std::vector<uint8_t> info;
    putBlockHeader(info, kStreamInfo, false, 34);
    putBigEndian(info, blockSize, 2);  // minimum block size
    putBigEndian(info, blockSize, 2);  // maximum block size
    putBigEndian(info, minFrameBytes_, 3);
    putBigEndian(info, maxFrameBytes_, 3);
    // sample rate (20 bits), channels - 1 (3), bits per sample - 1 (5), total samples (36)
    putBigEndian(info, uint64_t(kSampleRate) << 44 | uint64_t(1) << 41 | uint64_t(15) << 36 | totalSamples_, 8);
    const std::array<uint8_t, 16> digest = md5_.finish();
    info.insert(info.end(), digest.begin(), digest.end());

    // SEEKTABLE (points on frame boundaries) followed by PADDING.
    std::vector<uint8_t> tail;
    if (totalSamples_ > 0) {
        uint64_t interval = kSeekInterval;
        if ((totalSamples_ + interval - 1) / interval > kMaxSeekPoints)
            interval = (totalSamples_ + kMaxSeekPoints - 1) / kMaxSeekPoints;
        std::vector<uint64_t> frames;  // frame indexes, ascending and unique
        for (uint64_t target = 0; target < totalSamples_; target += interval) {
            const uint64_t frame = target / flac::kBlockSize;
            if (frames.empty() || frames.back() != frame) frames.push_back(frame);
        }
        putBlockHeader(tail, kSeekTable, false, uint32_t(frames.size() * 18));
        for (uint64_t frame : frames) {
            const uint64_t first = frame * flac::kBlockSize;
            putBigEndian(tail, first, 8);
            putBigEndian(tail, frameOffsets_[size_t(frame)], 8);
            putBigEndian(tail, std::min<uint64_t>(flac::kBlockSize, totalSamples_ - first), 2);
        }
    }
    putBlockHeader(tail, kPadding, true, uint32_t(kReservedBytes - tail.size() - 4));
    tail.resize(kReservedBytes);

    out_.seekp(4);
    writeBytes(info.data(), info.size());
    out_.seekp(std::streamoff(reservedOffset_));
    writeBytes(tail.data(), tail.size());
    if (leadOutOffsetPos_) {  // the lead-out is where the audio actually ends
        std::vector<uint8_t> leadOut;
        putBigEndian(leadOut, totalSamples_, 8);
        out_.seekp(std::streamoff(leadOutOffsetPos_));
        writeBytes(leadOut.data(), leadOut.size());
    }
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize FLAC file");
}

}  // namespace cdr
