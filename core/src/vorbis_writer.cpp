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

struct VorbisWriter::State {
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
    // The comment header with our tags; the vendor string stays libvorbis'.
    std::vector<uint8_t> tags = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    const std::vector<uint8_t> body = vorbisComment(metadata, commentVendor(comment));
    tags.insert(tags.end(), body.begin(), body.end());
    tags.push_back(1);  // framing bit

    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        state_.reset();
        throw std::runtime_error("cannot create " + path.u8string());
    }
    partial_.clear();
    samples_ = 0;
    uint32_t serial = 0x56524231u;  // "VRB1"
    for (char c : metadata.title + metadata.album) serial = serial * 31 + uint8_t(c);
    serial ^= uint32_t(metadata.trackNumber) << 24;
    ogg_ = std::make_unique<OggStreamWriter>(serial, [this](const uint8_t* page, size_t bytes) { writeBytes(page, bytes); });

    // The identification header alone on the first page; the comment and
    // setup headers follow, and the audio starts on a fresh page.
    ogg_->writePacket(header.packet, size_t(header.bytes), 0);
    ogg_->flush();
    ogg_->writePacket(tags, 0);
    ogg_->writePacket(codebooks.packet, size_t(codebooks.bytes), 0);
    ogg_->flush();
}

void VorbisWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!out_.is_open() || !state_) throw std::runtime_error("Vorbis file is not open");
    auto sample = [](const uint8_t* p) { return float(int16_t(uint16_t(p[0] | p[1] << 8))) / 32768.0f; };
    if (!partial_.empty()) {
        const size_t used = std::min(bytes, kBytesPerFrame - partial_.size());
        partial_.insert(partial_.end(), pcm, pcm + used);
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
        drain();
    }
    partial_.assign(pcm, pcm + bytes);
}

// Writes the packets libvorbis has ready; the last one carries e_o_s.
void VorbisWriter::drain() {
    ogg_packet packet;
    while (vorbis_analysis_blockout(&state_->dsp, &state_->block) == 1) {
        if (vorbis_analysis(&state_->block, nullptr) != 0 || vorbis_bitrate_addblock(&state_->block) != 0)
            throw std::runtime_error("Vorbis encoder error");
        while (vorbis_bitrate_flushpacket(&state_->dsp, &packet) == 1)
            ogg_->writePacket(packet.packet, size_t(packet.bytes), packet.granulepos, packet.e_o_s != 0);
    }
}

void VorbisWriter::writeBytes(const uint8_t* data, size_t size) {
    out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    if (!out_) throw std::runtime_error("write error while saving Vorbis data");
}

void VorbisWriter::close() {
    if (!out_.is_open()) return;
    try {
        if (!partial_.empty()) throw std::runtime_error("Vorbis input ends in the middle of a sample");
        vorbis_analysis_wrote(&state_->dsp, 0);  // end of input
        drain();
        if (!ogg_->finished()) throw std::runtime_error("Vorbis encoder did not end the stream");
    } catch (...) {
        out_.close();
        state_.reset();
        throw;
    }
    state_.reset();
    out_.close();
    if (out_.fail()) throw std::runtime_error("failed to finalize Vorbis file");
}

}  // namespace cdr
