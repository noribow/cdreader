#include "cdreader/alac_writer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

#include "cdreader/mp4.h"

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace {

constexpr unsigned kBytesPerFrame = 4;  // one 16-bit stereo sample
constexpr size_t kFrameBytes = size_t(alac::kFrameLength) * kBytesPerFrame;

// The mdat header area: a "wide" placeholder box followed by the 8-byte
// mdat header. Should the media data outgrow 32-bit sizes, the two become
// one 16-byte mdat header with a 64-bit size (QuickTime convention).
constexpr size_t kMdatHeaderBytes = 16;

}  // namespace

AlacWriter::AlacWriter(alac::EncoderOptions options) : options_(options), encoder_(options) {}

AlacWriter::~AlacWriter() {
    try {
        close();
    } catch (...) {
    }
}

void AlacWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    metadata_ = metadata;
    pending_.clear();
    frameSizes_.clear();
    position_ = totalSamples_ = 0;
    escapedFrames_ = 0;
    encoder_ = alac::FrameEncoder(options_);  // fresh predictor state per file

    std::vector<uint8_t> head = mp4::fileType();
    mdatOffset_ = head.size();
    mp4::putBe(head, 8, 4);
    head.insert(head.end(), {'w', 'i', 'd', 'e'});
    mp4::putBe(head, 0, 4);  // mdat size, patched in close()
    head.insert(head.end(), {'m', 'd', 'a', 't'});
    writeBytes(head.data(), head.size());
}

void AlacWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open()) throw std::runtime_error("M4A file is not open");
    size_t used = 0;
    if (!pending_.empty()) {
        used = std::min(bytes, kFrameBytes - pending_.size());
        pending_.insert(pending_.end(), pcm, pcm + used);
        if (pending_.size() < kFrameBytes) return;
        encodeFrame(pending_.data(), alac::kFrameLength);
        pending_.clear();
    }
    for (; bytes - used >= kFrameBytes; used += kFrameBytes) encodeFrame(pcm + used, alac::kFrameLength);
    pending_.insert(pending_.end(), pcm + used, pcm + bytes);
}

void AlacWriter::encodeFrame(const uint8_t* pcm, unsigned samples) {
    left_.resize(samples);
    right_.resize(samples);
    for (unsigned i = 0; i < samples; ++i) {
        const uint8_t* p = pcm + size_t(i) * kBytesPerFrame;
        left_[i] = int16_t(uint16_t(p[0] | p[1] << 8));
        right_[i] = int16_t(uint16_t(p[2] | p[3] << 8));
    }
    const std::vector<uint8_t> frame = encoder_.encode(left_.data(), right_.data(), samples);
    if (encoder_.lastFrame().escaped) ++escapedFrames_;
    writeBytes(frame.data(), frame.size());
    frameSizes_.push_back(uint32_t(frame.size()));
    totalSamples_ += samples;
}

void AlacWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving M4A data");
    position_ += size;
}

void AlacWriter::close() {
    if (!out_.is_open()) return;
    if (pending_.size() % kBytesPerFrame) {
        out_.close();
        throw std::runtime_error("M4A input ends in the middle of a sample");
    }
    if (!pending_.empty()) encodeFrame(pending_.data(), unsigned(pending_.size() / kBytesPerFrame));
    pending_.clear();

    // Frames follow each other from the end of the mdat header.
    mp4::AudioTrack track;
    track.sampleRate = alac::kSampleRate;
    track.duration = totalSamples_;
    track.frameDuration = alac::kFrameLength;
    track.frameSizes = frameSizes_;
    uint64_t offset = mdatOffset_ + kMdatHeaderBytes;
    uint64_t mediaBytes = 0;
    uint32_t maxFrameBytes = 0;
    for (uint32_t size : frameSizes_) {
        track.frameOffsets.push_back(offset);
        offset += size;
        mediaBytes += size;
        maxFrameBytes = std::max(maxFrameBytes, size);
    }

    alac::SpecificConfig config;
    config.maxFrameBytes = maxFrameBytes;
    if (totalSamples_ > 0) {
        const uint64_t bitRate = mediaBytes * 8 * alac::kSampleRate / totalSamples_;
        config.avgBitRate = uint32_t(std::min<uint64_t>(bitRate, std::numeric_limits<uint32_t>::max()));
    }
    const std::array<uint8_t, 24> cookie = alac::magicCookie(config);
    const std::vector<uint8_t> alacBox =
        mp4::fullBox("alac", 0, 0, std::vector<uint8_t>(cookie.begin(), cookie.end()));
    track.sampleEntry = mp4::audioSampleEntry("alac", alac::kChannels, alac::kBitDepth, alac::kSampleRate, alacBox);

    const std::vector<uint8_t> moov =
        mp4::movie(track, mp4::itunesMetadata(metadata_, "cdreader " CDREADER_VERSION));
    writeBytes(moov.data(), moov.size());

    std::vector<uint8_t> header;
    if (mediaBytes + 8 <= std::numeric_limits<uint32_t>::max()) {
        mp4::putBe(header, mediaBytes + 8, 4);
        out_.seekp(std::streamoff(mdatOffset_ + 8));
    } else {
        mp4::putBe(header, 1, 4);  // 64-bit size follows the type
        header.insert(header.end(), {'m', 'd', 'a', 't'});
        mp4::putBe(header, mediaBytes + 16, 8);
        out_.seekp(std::streamoff(mdatOffset_));
    }
    writeBytes(header.data(), header.size());
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize M4A file");
}

}  // namespace cdr
