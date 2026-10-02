#include "cdreader/accuraterip.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace cdr {

namespace {

constexpr const char* kDatabaseUrl = "http://www.accuraterip.com/accuraterip/";
constexpr size_t kHeaderBytes = 13;  // track count, id1, id2, cddb id
constexpr size_t kEntryBytes = 9;    // confidence, checksum, frame 450 checksum

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Whether `track` is the first / last audio track of the disc: AccurateRip
// skips samples there.
struct DiscEdges {
    bool first = false;
    bool last = false;
};

DiscEdges discEdges(const Toc& toc, const Track& track) {
    const Track* firstAudio = nullptr;
    const Track* lastAudio = nullptr;
    for (const Track& t : toc.tracks) {
        if (!t.isAudio) continue;
        if (!firstAudio) firstAudio = &t;
        lastAudio = &t;
    }
    return {firstAudio && firstAudio->number == track.number, lastAudio && lastAudio->number == track.number};
}

// The 1-based sample positions [first, last] that a track's checksum sums up.
uint32_t windowFirst(bool firstTrack) { return firstTrack ? kAccurateRipSkipSamples : 1; }

uint32_t windowLast(uint32_t trackSamples, bool lastTrack) {
    if (!lastTrack) return trackSamples;
    return trackSamples > kAccurateRipSkipSamples ? trackSamples - kAccurateRipSkipSamples : 0;
}

// Splits a PCM chunk into 32-bit samples, carrying an incomplete sample over
// to the next chunk in `partial`.
template <typename Fn>
void forEachSample(uint8_t (&partial)[4], size_t& partialBytes, const uint8_t* pcm, size_t bytes, Fn&& fn) {
    size_t i = 0;
    if (partialBytes) {
        while (partialBytes < 4 && i < bytes) partial[partialBytes++] = pcm[i++];
        if (partialBytes < 4) return;
        fn(le32(partial));
        partialBytes = 0;
    }
    for (; i + 4 <= bytes; i += 4) fn(le32(pcm + i));
    while (i < bytes) partial[partialBytes++] = pcm[i++];
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

}  // namespace

// id1 = sum of the audio track offsets + lead-out; id2 = the same with each
// offset weighted by its index among the audio tracks (an offset of 0 counts
// as 1). Offsets are LBAs where the first track of a normal disc starts at 0.
AccurateRipDiscId AccurateRipDiscId::fromToc(const Toc& toc) {
    AccurateRipDiscId id;
    uint32_t index = 0;
    for (const Track& t : toc.tracks) {
        if (!t.isAudio) continue;
        ++index;
        id.id1 += t.startLba;
        id.id2 += std::max<uint32_t>(t.startLba, 1) * index;
    }
    id.audioTracks = int(index);
    id.id1 += toc.leadOutLba;
    id.id2 += std::max<uint32_t>(toc.leadOutLba, 1) * (index + 1);
    id.cddb = toc.cddbId();
    return id;
}

std::string AccurateRipDiscId::toString() const {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%03d-%s-%s-%s", audioTracks, hex8(id1).c_str(), hex8(id2).c_str(),
                  hex8(cddb).c_str());
    return buf;
}

// The file lives in directories named after the last three hex digits of id1.
std::string AccurateRipDiscId::url() const {
    const std::string a = hex8(id1);
    return std::string(kDatabaseUrl) + a[7] + "/" + a[6] + "/" + a[5] + "/dBAR-" + toString() + ".bin";
}

AccurateRipChecksum::AccurateRipChecksum(uint32_t trackSamples, bool firstTrack, bool lastTrack)
    : first_(windowFirst(firstTrack)), last_(windowLast(trackSamples, lastTrack)) {}

AccurateRipChecksum AccurateRipChecksum::forTrack(const Toc& toc, const Track& track) {
    const DiscEdges e = discEdges(toc, track);
    return AccurateRipChecksum(track.lengthSectors * kSamplesPerSector, e.first, e.last);
}

// Sample n (1-based) contributes n * sample: v1 keeps the low 32 bits of each
// product, v2 additionally adds the high 32 bits.
void AccurateRipChecksum::add(uint32_t sample) {
    ++position_;
    if (position_ < first_ || position_ > last_) return;
    const uint64_t product = uint64_t(sample) * position_;
    lo_ += uint32_t(product);
    hi_ += uint32_t(product >> 32);
}

void AccurateRipChecksum::update(const uint8_t* pcm, size_t bytes) {
    forEachSample(partial_, partialBytes_, pcm, bytes, [this](uint32_t sample) { add(sample); });
}

std::vector<AccurateRipPressing> parseAccurateRipResponse(const std::string& body) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(body.data());
    const size_t size = body.size();
    std::vector<AccurateRipPressing> pressings;
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < kHeaderBytes) throw std::runtime_error("truncated AccurateRip record header");
        AccurateRipPressing p;
        p.id.audioTracks = data[pos];
        p.id.id1 = le32(data + pos + 1);
        p.id.id2 = le32(data + pos + 5);
        p.id.cddb = le32(data + pos + 9);
        pos += kHeaderBytes;
        if (p.id.audioTracks == 0) throw std::runtime_error("AccurateRip record without tracks");
        if (size - pos < size_t(p.id.audioTracks) * kEntryBytes)
            throw std::runtime_error("truncated AccurateRip record");
        for (int t = 0; t < p.id.audioTracks; ++t, pos += kEntryBytes) {
            AccurateRipEntry e;
            e.confidence = data[pos];
            e.checksum = le32(data + pos + 1);
            e.frame450Checksum = le32(data + pos + 5);
            p.tracks.push_back(e);
        }
        pressings.push_back(std::move(p));
    }
    return pressings;
}

AccurateRipLookup lookupAccurateRip(HttpClient& http, const AccurateRipDiscId& id) {
    AccurateRipLookup lookup;
    HttpResponse response;
    try {
        response = http.get(id.url());
    } catch (const std::exception& e) {
        lookup.error = e.what();
        return lookup;
    }
    if (!response.ok) {
        lookup.error = response.error.empty() ? "request failed" : response.error;
        return lookup;
    }
    if (response.status == 404) {
        lookup.status = AccurateRipLookup::Status::NotFound;
        return lookup;
    }
    if (response.status != 200) {
        lookup.error = "unexpected HTTP status " + std::to_string(response.status);
        return lookup;
    }
    try {
        lookup.pressings = parseAccurateRipResponse(response.body);
    } catch (const std::exception& e) {
        lookup.error = e.what();
        return lookup;
    }
    if (lookup.pressings.empty()) {
        lookup.error = "empty AccurateRip response";
        return lookup;
    }
    for (const AccurateRipPressing& p : lookup.pressings) {
        if (p.id != id) {
            lookup.pressings.clear();
            lookup.error = "AccurateRip response is for another disc (" + p.id.toString() + ")";
            return lookup;
        }
    }
    lookup.status = AccurateRipLookup::Status::Found;
    return lookup;
}

// Entries with zero confidence are placeholders (e.g. the data track slot of
// a Mixed Mode CD, or tracks too short to checksum) and never match.
AccurateRipTrackResult matchAccurateRip(const std::vector<AccurateRipPressing>& pressings, size_t entryIndex,
                                        int trackNumber, uint32_t v1, uint32_t v2) {
    AccurateRipTrackResult r;
    r.track = trackNumber;
    r.v1 = v1;
    r.v2 = v2;
    for (size_t i = 0; i < pressings.size(); ++i) {
        const AccurateRipPressing& p = pressings[i];
        if (entryIndex >= p.tracks.size()) continue;
        const AccurateRipEntry& e = p.tracks[entryIndex];
        if (e.confidence == 0) continue;
        AccurateRipPressingMatch m;
        m.pressing = int(i) + 1;
        m.checksum = e.checksum;
        m.confidence = e.confidence;
        r.totalConfidence += e.confidence;
        if (e.checksum == v2) {
            m.version = 2;
            r.v2Confidence += e.confidence;
        } else if (e.checksum == v1) {
            m.version = 1;
            r.v1Confidence += e.confidence;
        }
        r.pressings.push_back(m);
    }
    return r;
}

size_t accurateRipEntryIndex(const Toc& toc, const Track& track) {
    for (size_t i = 0; i < toc.tracks.size(); ++i)
        if (toc.tracks[i].number == track.number) return i;
    return toc.tracks.size();
}

AccurateRipOffsetScan::AccurateRipOffsetScan(uint32_t trackSamples, bool firstTrack, bool lastTrack,
                                             uint32_t maxOffset)
    : trackSamples_(trackSamples),
      maxOffset_(maxOffset),
      first_(windowFirst(firstTrack)),
      last_(windowLast(trackSamples, lastTrack)) {
    samples_.reserve(size_t(samplesNeeded()));
}

AccurateRipOffsetScan AccurateRipOffsetScan::forTrack(const Toc& toc, const Track& track, uint32_t maxOffset) {
    const DiscEdges e = discEdges(toc, track);
    return AccurateRipOffsetScan(track.lengthSectors * kSamplesPerSector, e.first, e.last, maxOffset);
}

void AccurateRipOffsetScan::update(const uint8_t* pcm, size_t bytes) {
    forEachSample(partial_, partialBytes_, pcm, bytes, [this](uint32_t sample) {
        if (samples_.size() < samplesNeeded()) samples_.push_back(sample);
    });
}

namespace {

constexpr uint64_t kResidueModulus = 0xFFFFFFFFu;  // 2^32 - 1

// Arithmetic for the sliding sums, either wrapping at 2^32 (v1) or modulo
// 2^32 - 1 (v2 residue).
struct Mod2_32 {
    static uint32_t add(uint32_t a, uint32_t b) { return a + b; }
    static uint32_t sub(uint32_t a, uint32_t b) { return a - b; }
    static uint32_t mul(uint32_t a, uint32_t b) { return a * b; }
};

struct ModResidue {
    static uint32_t add(uint32_t a, uint32_t b) { return uint32_t((uint64_t(a) + b) % kResidueModulus); }
    static uint32_t sub(uint32_t a, uint32_t b) {
        return uint32_t((uint64_t(a) + kResidueModulus - b % kResidueModulus) % kResidueModulus);
    }
    static uint32_t mul(uint32_t a, uint32_t b) { return uint32_t(uint64_t(a) * b % kResidueModulus); }
};

}  // namespace

// sum(position * sample) over the window [first, last] for every offset.
// With e = the stream and X_j / S_j the weighted / plain window sums at
// offset index j (position k reads stream sample k - 1 + j):
//   S_{j+1} = S_j - e[first - 1 + j] + e[last + j]
//   X_{j+1} = X_j - first * e[first - 1 + j] + (last + 1) * e[last + j] - S_{j+1}
template <typename Ring, typename At>
static std::vector<uint32_t> slidingSums(uint32_t first, uint32_t last, uint32_t offsets, At at) {
    std::vector<uint32_t> out(offsets, 0);
    if (last < first || offsets == 0) return out;  // track too short: nothing is summed
    uint32_t x = 0;
    uint32_t sum = 0;
    for (uint64_t k = first; k <= last; ++k) {
        x = Ring::add(x, Ring::mul(uint32_t(k), at(k - 1)));
        sum = Ring::add(sum, at(k - 1));
    }
    out[0] = x;
    for (uint32_t j = 0; j + 1 < offsets; ++j) {
        const uint32_t leaving = at(uint64_t(first) - 1 + j);
        const uint32_t entering = at(uint64_t(last) + j);
        sum = Ring::add(Ring::sub(sum, leaving), entering);
        x = Ring::sub(Ring::add(Ring::sub(x, Ring::mul(first, leaving)), Ring::mul(last + 1, entering)), sum);
        out[j + 1] = x;
    }
    return out;
}

std::vector<uint32_t> AccurateRipOffsetScan::checksums() const {
    return slidingSums<Mod2_32>(first_, last_, 2 * maxOffset_ + 1, [this](uint64_t i) { return sampleAt(i); });
}

std::vector<uint32_t> AccurateRipOffsetScan::v2Residues() const {
    return slidingSums<ModResidue>(first_, last_, 2 * maxOffset_ + 1,
                                   [this](uint64_t i) { return uint32_t(sampleAt(i) % kResidueModulus); });
}

// v2 = T mod 2^32 with T = sum(lo + hi) over the window. T == C + q * 2^32
// for C = v2, and since 2^32 == 1 (mod 2^32 - 1): residue == C + q. q is
// bounded because lo < 2^32 and hi = floor(sample * k / 2^32) < k, so a
// match needs (residue - C) mod (2^32 - 1) <= n + n * last / 2^32.
bool AccurateRipOffsetScan::mayMatchV2(uint32_t residue, uint32_t checksum) const {
    if (last_ < first_) return checksum == 0;
    const uint64_t n = uint64_t(last_) - first_ + 1;
    const uint64_t bound = n + (n * last_ >> 32) + 1;
    const uint64_t q = (uint64_t(residue) + kResidueModulus - checksum % kResidueModulus) % kResidueModulus;
    return q <= bound;
}

uint32_t AccurateRipOffsetScan::v2Checksum(int offset) const {
    if (last_ < first_) return 0;
    const uint64_t shift = uint64_t(int64_t(offset) + maxOffset_);
    uint32_t v2 = 0;
    for (uint64_t k = first_; k <= last_; ++k) {
        const uint64_t product = uint64_t(sampleAt(k - 1 + shift)) * k;
        v2 += uint32_t(product) + uint32_t(product >> 32);
    }
    return v2;
}

// The track is read starting maxOffset samples early and extended by
// 2 * maxOffset samples. The ripper does not read past the end of the audio
// session (most drives cannot), it fills that part with silence, as a rip
// with such an offset does. (The span must be extended even for the last
// track: it starts maxOffset samples early, so without the extension the
// last maxOffset readable samples would be missing, #37.)
AccurateRipOffsetScan scanReadOffsets(CdDrive& drive, const Toc& toc, const Track& track, uint32_t maxOffset,
                                      RipOptions options, const Ripper::Progress& progress) {
    AccurateRipOffsetScan scan = AccurateRipOffsetScan::forTrack(toc, track, maxOffset);
    const uint32_t wanted = (2 * maxOffset + kSamplesPerSector - 1) / kSamplesPerSector;
    Track span = track;
    span.lengthSectors += wanted;
    options.readOffsetSamples = -int(maxOffset);
    Ripper ripper(drive, toc, options);
    ripper.ripTrack(span, [&](const uint8_t* pcm, size_t bytes) { scan.update(pcm, bytes); }, progress);
    return scan;
}

std::vector<AccurateRipOffsetMatch> findAccurateRipOffsets(const AccurateRipOffsetScan& scan,
                                                           const std::vector<AccurateRipPressing>& pressings,
                                                           size_t entryIndex) {
    std::vector<const AccurateRipEntry*> entries;
    for (const AccurateRipPressing& p : pressings)
        if (entryIndex < p.tracks.size() && p.tracks[entryIndex].confidence != 0)
            entries.push_back(&p.tracks[entryIndex]);
    std::vector<AccurateRipOffsetMatch> matches;
    if (entries.empty()) return matches;

    const std::vector<uint32_t> v1 = scan.checksums();
    const std::vector<uint32_t> residues = scan.v2Residues();
    for (size_t i = 0; i < v1.size(); ++i) {
        AccurateRipOffsetMatch m;
        m.offset = int(i) - int(scan.maxOffset());
        bool haveV2 = false;
        uint32_t v2 = 0;
        for (const AccurateRipEntry* e : entries) {
            // As in matchAccurateRip(): an entry equal to both counts as v2.
            if (scan.mayMatchV2(residues[i], e->checksum)) {
                if (!haveV2) {
                    v2 = scan.v2Checksum(m.offset);
                    haveV2 = true;
                }
                if (v2 == e->checksum) {
                    m.v2Confidence += e->confidence;
                    ++m.pressings;
                    continue;
                }
            }
            if (v1[i] == e->checksum) {
                m.v1Confidence += e->confidence;
                ++m.pressings;
            }
        }
        if (m.confidence() > 0) matches.push_back(m);
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const AccurateRipOffsetMatch& a, const AccurateRipOffsetMatch& b) {
                         return a.confidence() > b.confidence();
                     });
    return matches;
}

static std::string versionName(int v1Confidence, int v2Confidence) {
    if (v1Confidence && v2Confidence) return "v1+v2";
    if (v2Confidence) return "v2";
    if (v1Confidence) return "v1";
    return "";
}

std::string AccurateRipOffsetMatch::matchedVersion() const { return versionName(v1Confidence, v2Confidence); }

std::string AccurateRipTrackResult::matchedVersion() const { return versionName(v1Confidence, v2Confidence); }

int AccurateRipTrackResult::matchingPressings() const {
    int n = 0;
    for (const AccurateRipPressingMatch& p : pressings) n += p.version != 0 ? 1 : 0;
    return n;
}

std::string AccurateRipTrackResult::describe() const {
    if (!inDatabase()) return "Not in database";
    const std::string counts = "v2 " + std::to_string(v2Confidence) + ", v1 " + std::to_string(v1Confidence) +
                               " of " + std::to_string(totalConfidence) + " submissions; " +
                               std::to_string(matchingPressings()) + " of " + std::to_string(pressings.size()) +
                               " pressings";
    if (!accurate()) return "Not accurate (" + counts + ")";
    return "Accurately ripped with " + matchedVersion() + " (" + counts + ")";
}

}  // namespace cdr
