#include "cdreader/mka_writer.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "cdreader/flac_encoder.h"
#include "cdreader/toc.h"
#ifdef CDREADER_HAVE_OPUS
#include "cdreader/opus_writer.h"
#endif
#ifdef CDREADER_HAVE_VORBIS
#include "cdreader/vorbis_writer.h"
#endif

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "dev"
#endif

namespace cdr {

namespace {

constexpr uint32_t kCdRate = 44100;
constexpr size_t kBytesPerFrame = 4;  // one 16-bit stereo sample

// Nanoseconds of `samples` at `rate`, rounded.
uint64_t toNs(uint64_t samples, uint32_t rate) { return (samples * 1000000000u + rate / 2) / rate; }

// FNV-1a: deterministic, non-zero UIDs (Matroska only requires them to be unique in the file).
uint64_t uidFrom(const std::string& text, uint64_t salt) {
    uint64_t h = 0xCBF29CE484222325ull ^ salt;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001B3ull;
    }
    return h ? h : 1;
}

std::string metadataKey(const TrackMetadata& m) {
    return m.discId + "\n" + m.album + "\n" + m.title + "\n" + std::to_string(m.trackNumber);
}

}  // namespace

// --- Codecs ------------------------------------------------------------------

struct MkaWriter::Encoder {
    virtual ~Encoder() = default;
    virtual std::string description() const = 0;
    // Starts a new stream; returns the track description.
    virtual mkv::AudioTrack start() = 0;
    virtual void write(const uint8_t* pcm, size_t bytes, mkv::Muxer& muxer) = 0;
    // Ends the stream; returns the duration in nanoseconds.
    virtual double finish(mkv::Muxer& muxer) = 0;
    virtual void abort() {}
};

namespace {

class MkaFlac : public MkaWriter::Encoder {
public:
    std::string description() const override { return "FLAC (built-in encoder) in Matroska, lossless"; }

    mkv::AudioTrack start() override {
        muxer_ = nullptr;
        position_ = 0;
        stream_.start([this](const std::vector<uint8_t>& frame, unsigned samples) {
            muxer_->addFrame(frame.data(), frame.size(), toNs(position_, kCdRate));
            position_ += samples;
        });
        mkv::AudioTrack t;
        t.codecId = "A_FLAC";
        t.bitDepth = 16;
        t.codecPrivate = codecPrivate();  // STREAMINFO placeholder, completed in finish()
        return t;
    }

    void write(const uint8_t* pcm, size_t bytes, mkv::Muxer& muxer) override {
        muxer_ = &muxer;
        stream_.write(pcm, bytes);
    }

    double finish(mkv::Muxer& muxer) override {
        muxer_ = &muxer;
        if (!stream_.wholeSamples()) throw std::runtime_error("FLAC input ends in the middle of a sample");
        stream_.finish();
        muxer.updateCodecPrivate(codecPrivate());
        return double(stream_.totalSamples()) * 1e9 / kCdRate;
    }

private:
    // "fLaC" and the STREAMINFO block, flagged as the last metadata block.
    std::vector<uint8_t> codecPrivate() const {
        std::vector<uint8_t> v = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
        const std::vector<uint8_t> info = stream_.streamInfo();
        v.insert(v.end(), info.begin(), info.end());
        return v;
    }

    flac::StreamEncoder stream_;
    mkv::Muxer* muxer_ = nullptr;  // receives the frames (set by write() / finish())
    uint64_t position_ = 0;        // samples given to the muxer so far
};

class MkaPcm : public MkaWriter::Encoder {
public:
    static constexpr size_t kBlockSamples = kCdRate / 10;  // 100 ms: exact millisecond timestamps
    static constexpr size_t kBlockBytes = kBlockSamples * kBytesPerFrame;

    std::string description() const override { return "PCM 16-bit little-endian in Matroska, uncompressed"; }

    mkv::AudioTrack start() override {
        pending_.clear();
        samples_ = 0;
        mkv::AudioTrack t;
        t.codecId = "A_PCM/INT/LIT";
        t.bitDepth = 16;
        t.defaultDurationNs = 100000000;
        return t;
    }

    void write(const uint8_t* pcm, size_t bytes, mkv::Muxer& muxer) override {
        while (bytes > 0) {
            const size_t n = std::min(bytes, kBlockBytes - pending_.size());
            pending_.insert(pending_.end(), pcm, pcm + n);
            pcm += n;
            bytes -= n;
            if (pending_.size() == kBlockBytes) emit(muxer);
        }
    }

    double finish(mkv::Muxer& muxer) override {
        if (pending_.size() % kBytesPerFrame) throw std::runtime_error("PCM input ends in the middle of a sample");
        if (!pending_.empty()) emit(muxer);
        return double(samples_) * 1e9 / kCdRate;
    }

private:
    void emit(mkv::Muxer& muxer) {
        muxer.addFrame(pending_.data(), pending_.size(), toNs(samples_, kCdRate));
        samples_ += pending_.size() / kBytesPerFrame;
        pending_.clear();
    }

    std::vector<uint8_t> pending_;
    uint64_t samples_ = 0;
};

#ifdef CDREADER_HAVE_OPUS
class MkaOpus : public MkaWriter::Encoder {
public:
    explicit MkaOpus(int bitrateKbps) : bitrateKbps_(bitrateKbps), encoder_(bitrateKbps) {}

    std::string description() const override {
        return "Opus (" + opus::libraryVersion() + ") in Matroska, VBR " + std::to_string(bitrateKbps_) +
               " kbit/s, 20 ms frames, resampled from 44.1 to 48 kHz";
    }

    mkv::AudioTrack start() override {
        encoder_.start();
        mkv::AudioTrack t;
        t.codecId = "A_OPUS";
        t.samplingFrequency = opus::kSampleRate;
        t.codecPrivate = opus::headPacket(2, encoder_.preSkip(), kCdRate);
        // The Matroska Opus mapping: CodecDelay = pre-skip, SeekPreRoll 80 ms.
        t.codecDelayNs = toNs(encoder_.preSkip(), opus::kSampleRate);
        t.seekPreRollNs = 80000000;
        t.defaultDurationNs = toNs(opus::kFrameSamples, opus::kSampleRate);
        return t;
    }

    void write(const uint8_t* pcm, size_t bytes, mkv::Muxer& muxer) override {
        encoder_.write(pcm, bytes, sink(muxer));
    }

    double finish(mkv::Muxer& muxer) override {
        encoder_.finish(sink(muxer));
        return double(encoder_.outputSamples()) * 1e9 / opus::kSampleRate;
    }

    void abort() override { encoder_.release(); }

private:
    opus::PacketEncoder::PacketSink sink(mkv::Muxer& muxer) {
        return [&muxer](const opus::PacketEncoder::Packet& p) {
            // Timestamps include the pre-skip (CodecDelay is subtracted on
            // playback); the padding after the end of the signal is discarded.
            const int64_t padding = p.last ? int64_t(toNs(p.end - p.granule, opus::kSampleRate)) : 0;
            muxer.addFrame(p.data, p.size, toNs(p.start, opus::kSampleRate), padding);
        };
    }

    int bitrateKbps_;
    opus::PacketEncoder encoder_;
};
#endif

#ifdef CDREADER_HAVE_VORBIS
// Xiph lacing (RFC 9559 section 10.3.2 / Matroska codec mappings): the
// number of packets - 1, the sizes of all but the last one as runs of 255,
// then the packets.
std::vector<uint8_t> xiphLaced(const std::vector<std::vector<uint8_t>>& packets) {
    std::vector<uint8_t> v = {uint8_t(packets.size() - 1)};
    for (size_t i = 0; i + 1 < packets.size(); ++i) {
        size_t n = packets[i].size();
        for (; n >= 255; n -= 255) v.push_back(255);
        v.push_back(uint8_t(n));
    }
    for (const std::vector<uint8_t>& p : packets) v.insert(v.end(), p.begin(), p.end());
    return v;
}

class MkaVorbis : public MkaWriter::Encoder {
public:
    // Throws std::invalid_argument for invalid settings (checked by VorbisWriter).
    MkaVorbis(std::optional<double> quality, std::optional<int> bitrateKbps)
        : description_(VorbisWriter(quality, bitrateKbps).encoderDescription()),
          encoder_(quality.value_or(vorbis::kDefaultQuality), bitrateKbps) {
        // "Vorbis (libvorbis), VBR ..." -> "Vorbis (libvorbis) in Matroska, VBR ..."
        const size_t comma = description_.find("), ");
        if (comma != std::string::npos) description_.insert(comma + 1, " in Matroska");
    }

    std::string description() const override { return description_; }

    mkv::AudioTrack start() override {
        // The tags are Matroska Tags; the comment header only has the vendor string.
        const vorbis::PacketEncoder::Headers h = encoder_.start(TrackMetadata{});
        mkv::AudioTrack t;
        t.codecId = "A_VORBIS";
        t.codecPrivate = xiphLaced({h.identification, h.comment, h.setup});
        return t;
    }

    void write(const uint8_t* pcm, size_t bytes, mkv::Muxer& muxer) override {
        encoder_.write(pcm, bytes, sink(muxer));
    }

    double finish(mkv::Muxer& muxer) override {
        const uint64_t samples = encoder_.samples();
        encoder_.finish(sink(muxer));
        return double(samples) * 1e9 / kCdRate;
    }

    void abort() override { encoder_.release(); }

private:
    vorbis::PacketEncoder::PacketSink sink(mkv::Muxer& muxer) {
        return [&muxer](const vorbis::PacketEncoder::Packet& p) {
            // Matroska has no granule positions: the end of the last packet
            // is trimmed with DiscardPadding.
            int64_t padding = 0;
            if (p.last && p.granule >= 0 && p.naturalEnd > uint64_t(p.granule))
                padding = int64_t(toNs(p.naturalEnd - uint64_t(p.granule), kCdRate));
            muxer.addFrame(p.data, p.size, toNs(p.start, kCdRate), padding);
        };
    }

    std::string description_;
    vorbis::PacketEncoder encoder_;
};
#endif

}  // namespace

// --- MkaWriter ---------------------------------------------------------------

bool MkaWriter::codecAvailable(Codec codec) {
    switch (codec) {
        case Codec::Flac:
        case Codec::Pcm: return true;
        case Codec::Opus:
#ifdef CDREADER_HAVE_OPUS
            return true;
#else
            return false;
#endif
        case Codec::Vorbis:
#ifdef CDREADER_HAVE_VORBIS
            return true;
#else
            return false;
#endif
    }
    return false;
}

MkaWriter::MkaWriter(Codec codec, const EncoderSettings& settings) : codec_(codec) {
    switch (codec) {
        case Codec::Flac:
        case Codec::Pcm:
            if (!settings.empty())
                throw std::invalid_argument(std::string(codec == Codec::Flac ? "FLAC" : "PCM") +
                                            " is lossless: a bitrate or quality does not apply");
            if (codec == Codec::Flac) encoder_ = std::make_unique<MkaFlac>();
            else encoder_ = std::make_unique<MkaPcm>();
            return;
        case Codec::Opus:
#ifdef CDREADER_HAVE_OPUS
        {
            if (settings.quality) throw std::invalid_argument("Opus takes a bitrate, not a quality");
            const OpusWriter check(settings.bitrateKbps.value_or(opus::kDefaultBitrateKbps));  // validates the range
            encoder_ = std::make_unique<MkaOpus>(check.bitrateKbps());
            return;
        }
#else
            throw std::invalid_argument("Opus is not available in this build (CDREADER_WITH_OPUS=OFF)");
#endif
        case Codec::Vorbis:
#ifdef CDREADER_HAVE_VORBIS
        {
            encoder_ = std::make_unique<MkaVorbis>(settings.quality, settings.bitrateKbps);
            return;
        }
#else
            throw std::invalid_argument("Vorbis is not available in this build (CDREADER_WITH_VORBIS=OFF)");
#endif
    }
    throw std::invalid_argument("unknown Matroska codec");
}

MkaWriter::~MkaWriter() {
    try {
        close();
    } catch (...) {
    }
}

std::string MkaWriter::encoderDescription() const { return encoder_->description(); }

bool MkaWriter::hasHtoaChapter(const EmbeddedCueSheet& cue) {
    if (cue.tracks.empty()) return false;
    const CueTrack& t = cue.tracks.front();
    return t.number == 1 && t.hasIndex00 && t.index00File.empty() && t.index00Sectors == 0 && t.startSectors > 0;
}

std::vector<mkv::Chapter> MkaWriter::chaptersFor(const EmbeddedCueSheet& cue, const TrackMetadata& album) {
    std::vector<mkv::Chapter> chapters;
    // 1/75 s per CD frame: 40000000/3 ns, rounded.
    auto ns = [](uint64_t sectors) { return (sectors * 40000000u + 1) / 3; };
    // An image that starts with the HTOA (#25: track 1 with INDEX 00 at the
    // start of the file) gets a leading "Hidden Track" chapter up to track 1's
    // INDEX 01, so the whole timeline is covered and the HTOA can be reached.
    if (hasHtoaChapter(cue)) {
        mkv::Chapter c;
        c.uid = uidFrom(metadataKey(album) + "\nchapter 0", 0);
        c.startNs = 0;
        c.endNs = ns(cue.tracks.front().startSectors);
        c.title = "Hidden Track";
        chapters.push_back(c);
    }
    for (size_t i = 0; i < cue.tracks.size(); ++i) {
        const CueTrack& t = cue.tracks[i];
        mkv::Chapter c;
        c.uid = uidFrom(metadataKey(album) + "\nchapter " + std::to_string(t.number), i + 1);
        c.startNs = ns(t.startSectors);
        c.endNs = ns(i + 1 < cue.tracks.size() ? cue.tracks[i + 1].startSectors : std::max(cue.totalSectors, t.startSectors));
        if (!t.title.empty()) {
            c.title = t.title;
        } else {
            char name[32];
            std::snprintf(name, sizeof name, "Track %02d", t.number);
            c.title = name;
        }
        chapters.push_back(c);
    }
    return chapters;
}

std::vector<mkv::Tag> MkaWriter::tagsFor(const TrackMetadata& m, const std::vector<mkv::Chapter>& chapters,
                                         const EmbeddedCueSheet* cue) {
    auto add = [](mkv::Tag& tag, const char* name, const std::string& value) {
        if (!value.empty()) tag.simpleTags.push_back({name, value});
    };
    std::vector<mkv::Tag> tags;

    mkv::Tag album;
    album.targetTypeValue = 50;
    add(album, "TITLE", m.album);
    add(album, "ALBUM", m.album);  // for software that ignores the target levels (FFmpeg)
    add(album, "ARTIST", m.albumArtist.empty() ? m.artist : m.albumArtist);
    if (m.trackTotal > 0) add(album, "TOTAL_PARTS", std::to_string(m.trackTotal));
    add(album, "DATE_RELEASED", m.year);
    add(album, "GENRE", m.genre);
    add(album, "CDDB", m.discId);
    add(album, "BARCODE", !m.mcn.empty() ? m.mcn : cue ? cue->mcn : std::string());  // MCN (UPC / EAN), #22
    if (!album.simpleTags.empty()) tags.push_back(album);

    if (cue) {
        // The HTOA chapter (if any) comes before the tracks' chapters.
        const size_t first = hasHtoaChapter(*cue) && chapters.size() > cue->tracks.size() ? 1 : 0;
        if (first) {
            mkv::Tag tag;
            tag.targetTypeValue = 30;
            tag.chapterUid = chapters[0].uid;
            add(tag, "TITLE", chapters[0].title);
            add(tag, "PART_NUMBER", "0");
            tags.push_back(tag);
        }
        for (size_t i = 0; i + first < chapters.size() && i < cue->tracks.size(); ++i) {
            const CueTrack& t = cue->tracks[i];
            mkv::Tag tag;
            tag.targetTypeValue = 30;
            tag.chapterUid = chapters[i + first].uid;
            add(tag, "TITLE", t.title);
            add(tag, "ARTIST", t.performer);
            add(tag, "PART_NUMBER", std::to_string(t.number));
            add(tag, "ISRC", t.isrc);
            tags.push_back(tag);
        }
    } else {
        // Written after the album tag: software that merges the levels keeps the track's TITLE / ARTIST.
        mkv::Tag track;
        track.targetTypeValue = 30;
        add(track, "TITLE", m.title);
        add(track, "ARTIST", m.artist);
        if (m.trackNumber > 0) add(track, "PART_NUMBER", std::to_string(m.trackNumber));
        add(track, "ISRC", m.isrc);
        if (!track.simpleTags.empty()) tags.push_back(track);
    }
    return tags;
}

void MkaWriter::open(const std::filesystem::path& path, const TrackMetadata& metadata) {
    close();
    mkv::AudioTrack track = encoder_->start();
    track.uid = uidFrom(metadataKey(metadata) + "\ntrack", 1);
    const std::vector<mkv::Chapter> chapters = cue_ ? chaptersFor(*cue_, metadata) : std::vector<mkv::Chapter>();
    const std::vector<mkv::Tag> tags = tagsFor(metadata, chapters, cue_ ? &*cue_ : nullptr);

    out_ = std::make_unique<std::ofstream>(path, std::ios::binary | std::ios::trunc);
    if (!*out_) {
        out_.reset();
        encoder_->abort();
        throw std::runtime_error("cannot create " + path.u8string());
    }
    muxer_ = std::make_unique<mkv::Muxer>(*out_);
    try {
        muxer_->begin("cdreader " CDREADER_VERSION, track, chapters, tags);
    } catch (...) {
        muxer_.reset();
        out_.reset();
        encoder_->abort();
        throw;
    }
}

void MkaWriter::write(const uint8_t* pcm, size_t bytes) {
    if (!muxer_) throw std::runtime_error("Matroska file is not open");
    encoder_->write(pcm, bytes, *muxer_);
}

void MkaWriter::close() {
    if (!muxer_) return;
    try {
        const double duration = encoder_->finish(*muxer_);
        muxer_->finish(duration);
    } catch (...) {
        muxer_.reset();
        out_.reset();
        encoder_->abort();
        throw;
    }
    muxer_.reset();
    out_->close();
    const bool failed = out_->fail();
    out_.reset();
    if (failed) throw std::runtime_error("failed to finalize Matroska file");
}

}  // namespace cdr
