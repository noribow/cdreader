#include "cdreader/opus_writer.h"

#include <opus.h>

#include <algorithm>
#include <stdexcept>

#include "cdreader/tags.h"

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace opus {

namespace {

constexpr uint32_t kCdSampleRate = 44100;
constexpr unsigned kChannels = 2;
constexpr unsigned kBytesPerFrame = 4;      // one 16-bit stereo sample
constexpr size_t kMaxPacketBytes = 4000;    // > 3 * 1275 + 7, the largest possible packet

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

std::vector<uint8_t> headPacket(uint8_t channels, uint16_t preSkip, uint32_t inputSampleRate) {
    std::vector<uint8_t> v = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, channels};
    put16(v, preSkip);
    put32(v, inputSampleRate);
    put16(v, 0);  // output gain (Q7.8 dB)
    v.push_back(0);  // channel mapping family 0: mono or stereo, no mapping table
    return v;
}

std::vector<uint8_t> tagsPacket(const TrackMetadata& metadata, const std::string& vendor) {
    std::vector<uint8_t> v = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    const std::vector<uint8_t> comment = vorbisComment(metadata, vendor);
    v.insert(v.end(), comment.begin(), comment.end());
    return v;
}

std::string libraryVersion() { return opus_get_version_string(); }

PacketEncoder::PacketEncoder(int bitrateKbps) : bitrateKbps_(bitrateKbps) {}

PacketEncoder::~PacketEncoder() { release(); }

void PacketEncoder::release() {
    if (encoder_) opus_encoder_destroy(encoder_);
    encoder_ = nullptr;
}

void PacketEncoder::start() {
    release();
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !encoder_) {
        encoder_ = nullptr;
        throw std::runtime_error(std::string("cannot create the Opus encoder: ") + opus_strerror(error));
    }
    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrateKbps_ * 1000)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_VBR_CONSTRAINT(0)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_SET_LSB_DEPTH(16)) != OPUS_OK ||
        opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0 ||
        lookahead > 65535) {
        release();
        throw std::runtime_error("cannot configure the Opus encoder");
    }
    preSkip_ = uint16_t(lookahead);
    resampler_ = std::make_unique<Resampler>(kCdSampleRate, kSampleRate, kChannels);
    partial_.clear();
    pending_.clear();
    havePacket_ = false;
    encodedSamples_ = finalGranule_ = 0;
}

void PacketEncoder::write(const uint8_t* pcm, size_t bytes, const PacketSink& sink) {
    if (!encoder_) throw std::runtime_error("the Opus encoder is not started");
    input_.clear();
    size_t used = 0;
    if (!partial_.empty()) {
        used = std::min(bytes, kBytesPerFrame - partial_.size());
        for (size_t i = 0; i < used; ++i) partial_.push_back(pcm[i]);  // (insert() trips -Wstringop-overflow)
        if (partial_.size() < kBytesPerFrame) return;
        input_.push_back(float(int16_t(uint16_t(partial_[0] | partial_[1] << 8))) / 32768.0f);
        input_.push_back(float(int16_t(uint16_t(partial_[2] | partial_[3] << 8))) / 32768.0f);
        partial_.clear();
    }
    for (; bytes - used >= kBytesPerFrame; used += kBytesPerFrame) {
        const uint8_t* p = pcm + used;
        input_.push_back(float(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768.0f);
        input_.push_back(float(int16_t(uint16_t(p[2] | p[3] << 8))) / 32768.0f);
    }
    partial_.assign(pcm + used, pcm + bytes);
    resampler_->process(input_.data(), input_.size() / kChannels, pending_);
    encodeAvailable(false, sink);
}

void PacketEncoder::finish(const PacketSink& sink) {
    if (!encoder_) throw std::runtime_error("the Opus encoder is not started");
    if (!partial_.empty()) {
        release();
        throw std::runtime_error("Opus input ends in the middle of a sample");
    }
    try {
        resampler_->finish(pending_);
        encodeAvailable(true, sink);
    } catch (...) {
        release();
        throw;
    }
    release();
}

// The packet waiting in packet_ ends at encodedSamples_.
void PacketEncoder::emit(const PacketSink& sink, uint64_t granule, bool last) {
    Packet p;
    p.data = packet_.data();
    p.size = packet_.size();
    p.end = encodedSamples_;
    p.start = encodedSamples_ - kFrameSamples;
    p.granule = granule;
    p.last = last;
    sink(p);
}

// Encodes every complete 20 ms frame of pending_. With `final`, the input
// has ended: the rest is padded with silence until the decoder output covers
// pre-skip + the whole signal, and the last packet is emitted with `last`.
void PacketEncoder::encodeAvailable(bool final, const PacketSink& sink) {
    const size_t frameValues = size_t(kFrameSamples) * kChannels;
    const uint64_t total = final ? uint64_t(preSkip_) + resampler_->outputFrames() : 0;
    size_t offset = 0;
    unsigned char packet[kMaxPacketBytes];
    for (;;) {
        if (pending_.size() - offset < frameValues) {
            if (!final || (encodedSamples_ >= total && havePacket_)) break;
            pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(offset));
            offset = 0;
            pending_.resize(frameValues, 0.0f);  // silence after the end
        }
        const opus_int32 size =
            opus_encode_float(encoder_, pending_.data() + offset, int(kFrameSamples), packet, opus_int32(sizeof packet));
        if (size < 0) throw std::runtime_error(std::string("Opus encoder error: ") + opus_strerror(size));
        offset += frameValues;
        if (havePacket_) emit(sink, encodedSamples_, false);
        packet_.assign(packet, packet + size);
        havePacket_ = true;
        encodedSamples_ += kFrameSamples;
    }
    pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(std::min(offset, pending_.size())));
    if (final) {
        // End trimming: the last granule position is where the signal ends,
        // which is inside the last packet.
        finalGranule_ = total;
        emit(sink, total, true);
        havePacket_ = false;
        pending_.clear();
    }
}

}  // namespace opus

using namespace opus;

OpusWriter::OpusWriter(int bitrateKbps) : bitrateKbps_(bitrateKbps), encoder_(bitrateKbps) {
    if (bitrateKbps < kMinBitrateKbps || bitrateKbps > kMaxBitrateKbps)
        throw std::invalid_argument("the Opus bitrate must be " + std::to_string(kMinBitrateKbps) + ".." +
                                    std::to_string(kMaxBitrateKbps) + " kbit/s");
}

OpusWriter::~OpusWriter() {
    try {
        close();
    } catch (...) {
    }
}

std::string OpusWriter::encoderDescription() const {
    return "Opus (" + libraryVersion() + "), VBR " + std::to_string(bitrateKbps_) +
           " kbit/s, 20 ms frames, resampled from 44.1 to 48 kHz";
}

void OpusWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    encoder_.start();
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        encoder_.release();
        throw std::runtime_error("cannot create " + path.u8string());
    }

    // A random serial number is recommended; one derived from the content is
    // deterministic, and a file only holds this one stream anyway.
    uint32_t serial = 0x43445231u;  // "CDR1"
    for (char c : metadata.title + metadata.album) serial = serial * 31 + uint8_t(c);
    serial ^= uint32_t(metadata.trackNumber) << 24;
    ogg_ = std::make_unique<OggStreamWriter>(serial, [this](const uint8_t* page, size_t bytes) { writeBytes(page, bytes); });

    // Each header packet ends its page (RFC 7845 section 3), granule position 0.
    ogg_->writePacket(headPacket(kChannels, encoder_.preSkip(), kCdSampleRate), 0);
    ogg_->flush();
    ogg_->writePacket(tagsPacket(metadata, "cdreader " CDREADER_VERSION " (" + libraryVersion() + ")"), 0);
    ogg_->flush();
}

void OpusWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open() || !encoder_.started()) throw std::runtime_error("Opus file is not open");
    encoder_.write(pcm, bytes, [this](const PacketEncoder::Packet& p) { writePacket(p); });
}

void OpusWriter::writePacket(const PacketEncoder::Packet& p) {
    ogg_->writePacket(p.data, p.size, int64_t(p.granule), p.last);
}

void OpusWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving Opus data");
}

void OpusWriter::close() {
    if (!out_.is_open()) return;
    try {
        encoder_.finish([this](const PacketEncoder::Packet& p) { writePacket(p); });
    } catch (...) {
        out_.close();
        throw;
    }
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize Opus file");
}

}  // namespace cdr
