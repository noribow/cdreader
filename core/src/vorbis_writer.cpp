#include "cdreader/vorbis_writer.h"

#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "cdreader/tags.h"

namespace cdr {

namespace vorbis {

std::string libraryVersion() { return vorbis_version_string(); }

}  // namespace vorbis

namespace {

constexpr long kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr size_t kBytesPerFrame = 4;
constexpr size_t kChunkFrames = 4096;

// Vendor string of a Vorbis comment header packet (0x03 "vorbis" length vendor ...).
std::string commentVendor(const ogg_packet& packet) {
    if (packet.bytes < 11) return {};
    const unsigned char* p = packet.packet + 7;
    const uint32_t length = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    if (length > uint32_t(packet.bytes - 11)) return {};
    return std::string(reinterpret_cast<const char*>(p + 4), length);
}

}  // namespace

namespace vorbis {

struct PacketEncoder::State {
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    bool dspReady = false;
    bool blockReady = false;

    State() {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }
    ~State() {
        if (blockReady) vorbis_block_clear(&block);
        if (dspReady) vorbis_dsp_clear(&dsp);
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

PacketEncoder::PacketEncoder(double quality, std::optional<int> bitrateKbps)
    : quality_(quality), bitrateKbps_(bitrateKbps) {}

PacketEncoder::~PacketEncoder() = default;

void PacketEncoder::release() { state_.reset(); }

PacketEncoder::Headers PacketEncoder::start(const TrackMetadata& metadata) {
    state_ = std::make_unique<State>();
    vorbis_info* vi = &state_->info;
    int result;
    if (bitrateKbps_) {
        // Like oggenc -b: a nominal bitrate with the bitrate management
        // engine off, i.e. VBR whose quality is chosen to average the bitrate.
        result = vorbis_encode_setup_managed(vi, kChannels, kSampleRate, -1, long(*bitrateKbps_) * 1000, -1);
        if (result == 0) result = vorbis_encode_ctl(vi, OV_ECTL_RATEMANAGE2_SET, nullptr);
        if (result == 0) result = vorbis_encode_setup_init(vi);
    } else {
        result = vorbis_encode_init_vbr(vi, kChannels, kSampleRate, float(quality_ / 10));
    }
    if (result != 0) {
        state_.reset();
        throw std::runtime_error("cannot set up the Vorbis encoder (error " + std::to_string(result) + ")");
    }
    if (vorbis_analysis_init(&state_->dsp, vi) != 0) {
        state_.reset();
        throw std::runtime_error("cannot start the Vorbis encoder");
    }
    state_->dspReady = true;
    vorbis_block_init(&state_->dsp, &state_->block);
    state_->blockReady = true;

    ogg_packet header, comment, codebooks;
    if (vorbis_analysis_headerout(&state_->dsp, &state_->comment, &header, &comment, &codebooks) != 0) {
        state_.reset();
        throw std::runtime_error("cannot create the Vorbis headers");
    }
    Headers h;
    h.identification.assign(header.packet, header.packet + header.bytes);
    // The comment header with our tags; the vendor string stays libvorbis'.
    h.comment = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    const std::vector<uint8_t> body = vorbisComment(metadata, commentVendor(comment));
    h.comment.insert(h.comment.end(), body.begin(), body.end());
    h.comment.push_back(1);  // framing bit
    h.setup.assign(codebooks.packet, codebooks.packet + codebooks.bytes);

    partial_.clear();
    samples_ = 0;
    lastGranule_ = 0;
    lastBlockSize_ = 0;
    return h;
}

void PacketEncoder::write(const uint8_t* pcm, size_t bytes, const PacketSink& sink) {
    if (!state_) throw std::runtime_error("the Vorbis encoder is not started");
    auto sample = [](const uint8_t* p) { return float(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768.0f; };
    if (!partial_.empty()) {
        const size_t used = std::min(bytes, kBytesPerFrame - partial_.size());
        for (size_t i = 0; i < used; ++i) partial_.push_back(pcm[i]);  // (insert() trips -Wstringop-overflow)
        pcm += used;
        bytes -= used;
        if (partial_.size() < kBytesPerFrame) return;
        float** buffer = vorbis_analysis_buffer(&state_->dsp, 1);
        buffer[0][0] = sample(partial_.data());
        buffer[1][0] = sample(partial_.data() + 2);
        vorbis_analysis_wrote(&state_->dsp, 1);
        ++samples_;
        partial_.clear();
    }
    while (bytes >= kBytesPerFrame) {
        const size_t frames = std::min(bytes / kBytesPerFrame, kChunkFrames);
        float** buffer = vorbis_analysis_buffer(&state_->dsp, int(frames));
        for (size_t i = 0; i < frames; ++i) {
            buffer[0][i] = sample(pcm + i * kBytesPerFrame);
            buffer[1][i] = sample(pcm + i * kBytesPerFrame + 2);
        }
        vorbis_analysis_wrote(&state_->dsp, int(frames));
        samples_ += frames;
        pcm += frames * kBytesPerFrame;
        bytes -= frames * kBytesPerFrame;
        drain(sink);
    }
    partial_.assign(pcm, pcm + bytes);
}

// Emits the packets libvorbis has ready; the last one carries e_o_s.
void PacketEncoder::drain(const PacketSink& sink) {
    ogg_packet packet;
    while (vorbis_analysis_blockout(&state_->dsp, &state_->block) == 1) {
        if (vorbis_analysis(&state_->block, nullptr) != 0 || vorbis_bitrate_addblock(&state_->block) != 0)
            throw std::runtime_error("Vorbis encoder error");
        while (vorbis_bitrate_flushpacket(&state_->dsp, &packet) == 1) {
            // A decoder returns nothing for the first packet, then a quarter
            // of the previous plus a quarter of the current block size.
            const long blockSize = vorbis_packet_blocksize(&state_->info, &packet);
            if (blockSize <= 0) throw std::runtime_error("Vorbis encoder produced an invalid packet");
            Packet p;
            p.data = packet.packet;
            p.size = size_t(packet.bytes);
            p.granule = packet.granulepos;
            p.last = packet.e_o_s != 0;
            p.start = lastGranule_;
            p.naturalEnd = p.start + (lastBlockSize_ ? uint64_t(lastBlockSize_ / 4 + blockSize / 4) : 0);
            lastBlockSize_ = blockSize;
            if (packet.granulepos > 0) lastGranule_ = uint64_t(packet.granulepos);
            sink(p);
        }
    }
}

void PacketEncoder::finish(const PacketSink& sink) {
    if (!state_) throw std::runtime_error("the Vorbis encoder is not started");
    try {
        if (!partial_.empty()) throw std::runtime_error("Vorbis input ends in the middle of a sample");
        vorbis_analysis_wrote(&state_->dsp, 0);  // end of input
        drain(sink);
    } catch (...) {
        state_.reset();
        throw;
    }
    state_.reset();
}

}  // namespace vorbis

VorbisWriter::VorbisWriter(std::optional<double> quality, std::optional<int> bitrateKbps)
    : bitrateKbps_(bitrateKbps) {
    if (quality && bitrateKbps) throw std::invalid_argument("Vorbis takes either a quality or a bitrate, not both");
    if (quality) {
        if (!(*quality >= vorbis::kMinQuality && *quality <= vorbis::kMaxQuality))
            throw std::invalid_argument("the Vorbis quality must be -1..10");
        quality_ = *quality;
    }
    if (bitrateKbps && (*bitrateKbps < vorbis::kMinBitrateKbps || *bitrateKbps > vorbis::kMaxBitrateKbps))
        throw std::invalid_argument("the Vorbis bitrate must be " + std::to_string(vorbis::kMinBitrateKbps) + ".." +
                                    std::to_string(vorbis::kMaxBitrateKbps) + " kbit/s");
}

VorbisWriter::~VorbisWriter() {
    try {
        close();
    } catch (...) {
    }
}

std::string VorbisWriter::encoderDescription() const {
    std::string mode;
    if (bitrateKbps_) {
        mode = "VBR, about " + std::to_string(*bitrateKbps_) + " kbit/s average";
    } else {
        char q[32];
        std::snprintf(q, sizeof q, "%g", quality_);
        mode = std::string("VBR quality ") + q;
    }
    return "Vorbis (" + vorbis::libraryVersion() + "), " + mode;
}

void VorbisWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    encoder_ = std::make_unique<vorbis::PacketEncoder>(quality_, bitrateKbps_);
    const vorbis::PacketEncoder::Headers headers = encoder_->start(metadata);

    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        encoder_.reset();
        throw std::runtime_error("cannot create " + path.u8string());
    }
    uint32_t serial = 0x56524231u;  // "VRB1"
    for (char c : metadata.title + metadata.album) serial = serial * 31 + uint8_t(c);
    serial ^= uint32_t(metadata.trackNumber) << 24;
    ogg_ = std::make_unique<OggStreamWriter>(serial, [this](const uint8_t* page, size_t bytes) { writeBytes(page, bytes); });

    // The identification header alone on the first page; the comment and
    // setup headers follow, and the audio starts on a fresh page.
    ogg_->writePacket(headers.identification, 0);
    ogg_->flush();
    ogg_->writePacket(headers.comment, 0);
    ogg_->writePacket(headers.setup, 0);
    ogg_->flush();
}

void VorbisWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open() || !encoder_ || !encoder_->started()) throw std::runtime_error("Vorbis file is not open");
    encoder_->write(pcm, bytes, [this](const vorbis::PacketEncoder::Packet& p) { writePacket(p); });
}

void VorbisWriter::writePacket(const vorbis::PacketEncoder::Packet& p) {
    ogg_->writePacket(p.data, p.size, p.granule, p.last);
}

void VorbisWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving Vorbis data");
}

void VorbisWriter::close() {
    if (!out_.is_open()) return;
    try {
        encoder_->finish([this](const vorbis::PacketEncoder::Packet& p) { writePacket(p); });
        if (!ogg_->finished()) throw std::runtime_error("Vorbis encoder did not end the stream");
    } catch (...) {
        out_.close();
        throw;
    }
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize Vorbis file");
}

}  // namespace cdr
