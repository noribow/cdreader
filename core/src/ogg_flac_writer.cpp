#include "cdreader/ogg_flac_writer.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace oggflac {

std::vector<uint8_t> firstPacket(uint16_t headerPackets, const std::vector<uint8_t>& streamInfo) {
    if (streamInfo.size() != flac::kStreamInfoBytes) throw std::invalid_argument("STREAMINFO must be 34 bytes");
    std::vector<uint8_t> p = {0x7F, 'F', 'L', 'A', 'C', 1, 0};  // packet type, signature, mapping version 1.0
    flac::putBigEndian(p, headerPackets, 2);
    p.insert(p.end(), {'f', 'L', 'a', 'C'});
    flac::putBlockHeader(p, flac::kStreamInfo, headerPackets == 0, flac::kStreamInfoBytes);
    p.insert(p.end(), streamInfo.begin(), streamInfo.end());
    return p;
}

}  // namespace oggflac

namespace {

// Position of the STREAMINFO body inside the first packet.
constexpr size_t kStreamInfoPos = oggflac::kFirstPacketBytes - flac::kStreamInfoBytes;

}  // namespace

OggFlacWriter::OggFlacWriter(flac::EncoderOptions options) : stream_(options) {}

OggFlacWriter::~OggFlacWriter() {
    try {
        close();
    } catch (...) {
    }
}

void OggFlacWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot create " + path.u8string());
    stream_.start([this](const std::vector<uint8_t>& frame, unsigned samples) { onFrame(frame, samples); });
    frame_.clear();
    haveFrame_ = false;
    framesEnd_ = 0;

    // One packet per metadata block after STREAMINFO; the last one is flagged as such.
    headers_.assign(1, {});
    const std::vector<uint8_t> tags =
        flac::vorbisComment(metadata, "cdreader " CDREADER_VERSION, cue_ ? flac::cueSheetTagText(cue_->text) : std::string());
    if (tags.size() >= (1u << 24)) throw std::runtime_error("FLAC tags too large");
    std::vector<uint8_t> comment;
    flac::putBlockHeader(comment, flac::kVorbisComment, !cue_, uint32_t(tags.size()));
    comment.insert(comment.end(), tags.begin(), tags.end());
    headers_.push_back(std::move(comment));
    leadOutOffsetPos_ = 0;
    if (cue_) {
        const std::vector<uint8_t> sheet = flac::cueSheet(*cue_, uint64_t(cue_->totalSectors) * kSamplesPerSector);
        std::vector<uint8_t> block;
        flac::putBlockHeader(block, flac::kCueSheet, true, uint32_t(sheet.size()));
        leadOutOffsetPos_ = block.size() + flac::cueSheetLeadOutOffsetPosition(cue_->tracks.size());
        block.insert(block.end(), sheet.begin(), sheet.end());
        headers_.push_back(std::move(block));
    }
    // STREAMINFO is written in close(); the placeholder has the same size.
    headers_[0] = oggflac::firstPacket(uint16_t(headers_.size() - 1),
                                       std::vector<uint8_t>(flac::kStreamInfoBytes, 0));

    // A random serial number is recommended; one derived from the content is
    // deterministic, and a file only holds this one stream anyway.
    serial_ = 0x43445246u;  // "CDRF"
    for (char c : metadata.title + metadata.album) serial_ = serial_ * 31 + uint8_t(c);
    serial_ ^= uint32_t(metadata.trackNumber) << 24;
    ogg_ = std::make_unique<OggStreamWriter>(serial_, [this](const uint8_t* page, size_t bytes) { writeBytes(page, bytes); });
    writeHeaders(*ogg_, false);
    headerBytes_ = uint64_t(out_.tellp());
}

void OggFlacWriter::writeHeaders(OggStreamWriter& ogg, bool endOfStream) const {
    // The first packet alone on the BOS page; the audio starts on a new page.
    // Header pages have granule position 0.
    ogg.writePacket(headers_[0], 0);
    ogg.flush();
    for (size_t i = 1; i < headers_.size(); ++i)
        ogg.writePacket(headers_[i], 0, endOfStream && i + 1 == headers_.size());
    if (!ogg.finished()) ogg.flush();
}

void OggFlacWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open()) throw std::runtime_error("Ogg FLAC file is not open");
    stream_.write(pcm, bytes);
}

// The previous frame is written once another one exists, so that the last
// one can carry the EOS flag.
void OggFlacWriter::onFrame(const std::vector<uint8_t>& frame, unsigned samples) {
    if (haveFrame_) ogg_->writePacket(frame_, int64_t(framesEnd_));
    frame_ = frame;
    haveFrame_ = true;
    framesEnd_ += samples;
}

void OggFlacWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving Ogg FLAC data");
}

void OggFlacWriter::close() {
    if (!out_.is_open()) return;
    if (!stream_.wholeSamples()) {
        out_.close();
        throw std::runtime_error("Ogg FLAC input ends in the middle of a sample");
    }
    stream_.finish();
    if (haveFrame_) ogg_->writePacket(frame_, int64_t(framesEnd_), true);
    haveFrame_ = false;

    const std::vector<uint8_t> info = stream_.streamInfo();
    std::copy(info.begin(), info.end(), headers_[0].begin() + std::ptrdiff_t(kStreamInfoPos));
    if (leadOutOffsetPos_) {  // the lead-out is where the audio actually ends
        std::vector<uint8_t> leadOut;
        flac::putBigEndian(leadOut, stream_.totalSamples(), 8);
        std::copy(leadOut.begin(), leadOut.end(), headers_.back().begin() + std::ptrdiff_t(leadOutOffsetPos_));
    }
    // The same packets give pages of the same sizes; only their contents and
    // CRCs change. Without audio, the last header page ends the stream.
    std::vector<uint8_t> pages;
    OggStreamWriter rebuilt(serial_, [&](const uint8_t* page, size_t bytes) { pages.insert(pages.end(), page, page + bytes); });
    writeHeaders(rebuilt, stream_.frames() == 0);
    if (pages.size() != headerBytes_) {
        out_.close();
        throw std::logic_error("Ogg FLAC header pages changed size");
    }
    out_.seekp(0);
    writeBytes(pages.data(), pages.size());
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize Ogg FLAC file");
}

}  // namespace cdr
