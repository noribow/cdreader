#include "cdreader/flac_writer.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace {

using flac::putBigEndian;
using flac::putBlockHeader;

// SEEKTABLE + PADDING share a fixed area reserved when the file is opened,
// because the number of seek points is only known at the end.
constexpr uint32_t kReservedBytes = 8192;
constexpr uint32_t kMaxSeekPoints = 100;
constexpr uint64_t kSeekInterval = 10 * flac::kSampleRate;  // a seek point every 10 s

}  // namespace

FlacWriter::FlacWriter(flac::EncoderOptions options) : stream_(options) {}

FlacWriter::~FlacWriter() {
    try {
        close();
    } catch (...) {
    }
}

void FlacWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    frameOffsets_.clear();
    audioBytes_ = 0;
    stream_.start([this](const std::vector<uint8_t>& frame, unsigned) {
        frameOffsets_.push_back(audioBytes_);
        writeBytes(frame.data(), frame.size());
        audioBytes_ += frame.size();
    });

    std::vector<uint8_t> head = {'f', 'L', 'a', 'C'};
    putBlockHeader(head, flac::kStreamInfo, false, flac::kStreamInfoBytes);
    head.resize(head.size() + flac::kStreamInfoBytes);  // written in close()
    const std::vector<uint8_t> tags =
        flac::vorbisComment(metadata, "cdreader " CDREADER_VERSION, cue_ ? flac::cueSheetTagText(cue_->text) : std::string());
    if (tags.size() >= (1u << 24)) throw std::runtime_error("FLAC tags too large");
    putBlockHeader(head, flac::kVorbisComment, false, uint32_t(tags.size()));
    head.insert(head.end(), tags.begin(), tags.end());
    leadOutOffsetPos_ = 0;
    if (cue_) {
        const std::vector<uint8_t> sheet =
            flac::cueSheet(*cue_, uint64_t(cue_->totalSectors) * kSamplesPerSector);
        putBlockHeader(head, flac::kCueSheet, false, uint32_t(sheet.size()));
        leadOutOffsetPos_ = head.size() + flac::cueSheetLeadOutOffsetPosition(cue_->tracks.size());
        head.insert(head.end(), sheet.begin(), sheet.end());
    }
    reservedOffset_ = head.size();
    putBlockHeader(head, flac::kPadding, true, kReservedBytes - 4);
    head.resize(head.size() + kReservedBytes - 4);
    writeBytes(head.data(), head.size());
}

void FlacWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open()) throw std::runtime_error("FLAC file is not open");
    stream_.write(pcm, bytes);
}

void FlacWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving FLAC data");
}

void FlacWriter::close() {
    if (!out_.is_open()) return;
    if (!stream_.wholeSamples()) {
        out_.close();
        throw std::runtime_error("FLAC input ends in the middle of a sample");
    }
    stream_.finish();
    const uint64_t totalSamples = stream_.totalSamples();
    std::vector<uint8_t> info;
    putBlockHeader(info, flac::kStreamInfo, false, flac::kStreamInfoBytes);
    const std::vector<uint8_t> body = stream_.streamInfo();
    info.insert(info.end(), body.begin(), body.end());

    // SEEKTABLE (points on frame boundaries) followed by PADDING.
    std::vector<uint8_t> tail;
    if (totalSamples > 0) {
        uint64_t interval = kSeekInterval;
        if ((totalSamples + interval - 1) / interval > kMaxSeekPoints)
            interval = (totalSamples + kMaxSeekPoints - 1) / kMaxSeekPoints;
        std::vector<uint64_t> frames;  // frame indexes, ascending and unique
        for (uint64_t target = 0; target < totalSamples; target += interval) {
            const uint64_t frame = target / flac::kBlockSize;
            if (frames.empty() || frames.back() != frame) frames.push_back(frame);
        }
        putBlockHeader(tail, flac::kSeekTable, false, uint32_t(frames.size() * 18));
        for (uint64_t frame : frames) {
            const uint64_t first = frame * flac::kBlockSize;
            putBigEndian(tail, first, 8);
            putBigEndian(tail, frameOffsets_[size_t(frame)], 8);
            putBigEndian(tail, std::min<uint64_t>(flac::kBlockSize, totalSamples - first), 2);
        }
    }
    putBlockHeader(tail, flac::kPadding, true, uint32_t(kReservedBytes - tail.size() - 4));
    tail.resize(kReservedBytes);

    out_.seekp(4);
    writeBytes(info.data(), info.size());
    out_.seekp(std::streamoff(reservedOffset_));
    writeBytes(tail.data(), tail.size());
    if (leadOutOffsetPos_) {  // the lead-out is where the audio actually ends
        std::vector<uint8_t> leadOut;
        putBigEndian(leadOut, totalSamples, 8);
        out_.seekp(std::streamoff(leadOutOffsetPos_));
        writeBytes(leadOut.data(), leadOut.size());
    }
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize FLAC file");
}

}  // namespace cdr
