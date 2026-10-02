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
    for (const AccurateRipPressing& p : pressings) {
        if (entryIndex >= p.tracks.size()) continue;
        const AccurateRipEntry& e = p.tracks[entryIndex];
        if (e.confidence == 0) continue;
        r.totalConfidence += e.confidence;
        if (e.checksum == v2) r.v2Confidence += e.confidence;
        else if (e.checksum == v1) r.v1Confidence += e.confidence;
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
      last_(windowLast(trackSamples, lastTrack)),
      leaving_(size_t(2) * maxOffset),
      entering_(size_t(2) * maxOffset) {}

AccurateRipOffsetScan AccurateRipOffsetScan::forTrack(const Toc& toc, const Track& track, uint32_t maxOffset) {
    const DiscEdges e = discEdges(toc, track);
    return AccurateRipOffsetScan(track.lengthSectors * kSamplesPerSector, e.first, e.last, maxOffset);
}

// At offset -maxOffset, track sample k (1-based) is stream sample k - 1; the
// checksum for that offset is accumulated directly. Each step to the next
// offset shifts the summed window [first, last] by one stream sample:
//   v1' = v1 - sum - (first - 1) * leaving + last * entering
// so only the samples leaving and entering the window need to be kept.
void AccurateRipOffsetScan::add(uint32_t sample) {
    const uint64_t i = index_++;
    const uint64_t k = i + 1;
    if (k >= first_ && k <= last_) {
        v1_ += uint32_t(k) * sample;
        sum_ += sample;
    }
    if (i + 1 >= first_ && i + 1 - first_ < leaving_.size()) leaving_[size_t(i + 1 - first_)] = sample;
    if (i >= last_ && i - last_ < entering_.size()) entering_[size_t(i - last_)] = sample;
}

void AccurateRipOffsetScan::update(const uint8_t* pcm, size_t bytes) {
    forEachSample(partial_, partialBytes_, pcm, bytes, [this](uint32_t sample) { add(sample); });
}

std::vector<uint32_t> AccurateRipOffsetScan::checksums() const {
    std::vector<uint32_t> out(size_t(2) * maxOffset_ + 1, 0);
    if (last_ < first_) return out;  // track too short: nothing is summed
    uint32_t v1 = v1_;
    uint32_t sum = sum_;
    out[0] = v1;
    for (size_t j = 0; j < leaving_.size(); ++j) {
        v1 = v1 - sum - (first_ - 1) * leaving_[j] + last_ * entering_[j];
        sum = sum - leaving_[j] + entering_[j];
        out[j + 1] = v1;
    }
    return out;
}

// The track is read starting maxOffset samples early and extended by
// 2 * maxOffset samples, but never past the end of its audio session, which
// most drives cannot read (the scan treats the missing tail as silence).
AccurateRipOffsetScan scanReadOffsets(CdDrive& drive, const Toc& toc, const Track& track, uint32_t maxOffset,
                                      RipOptions options, const Ripper::Progress& progress) {
    AccurateRipOffsetScan scan = AccurateRipOffsetScan::forTrack(toc, track, maxOffset);
    const uint32_t readableEnd = toc.audioRange(track).end;
    const uint32_t wanted = (2 * maxOffset + kSamplesPerSector - 1) / kSamplesPerSector;
    Track span = track;
    span.lengthSectors += std::min(wanted, readableEnd - track.endLba());
    options.readOffsetSamples = -int(maxOffset);
    Ripper ripper(drive, toc, options);
    ripper.ripTrack(span, [&](const uint8_t* pcm, size_t bytes) { scan.update(pcm, bytes); }, progress);
    return scan;
}

std::vector<AccurateRipOffsetMatch> findAccurateRipOffsets(const AccurateRipOffsetScan& scan,
                                                           const std::vector<AccurateRipPressing>& pressings,
                                                           size_t entryIndex) {
    const std::vector<uint32_t> sums = scan.checksums();
    std::vector<AccurateRipOffsetMatch> matches;
    for (size_t i = 0; i < sums.size(); ++i) {
        AccurateRipOffsetMatch m;
        m.offset = int(i) - int(scan.maxOffset());
        for (const AccurateRipPressing& p : pressings) {
            if (entryIndex >= p.tracks.size()) continue;
            const AccurateRipEntry& e = p.tracks[entryIndex];
            if (e.confidence != 0 && e.checksum == sums[i]) m.confidence += e.confidence;
        }
        if (m.confidence > 0) matches.push_back(m);
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const AccurateRipOffsetMatch& a, const AccurateRipOffsetMatch& b) {
                         return a.confidence > b.confidence;
                     });
    return matches;
}

std::string AccurateRipTrackResult::describe() const {
    if (!inDatabase()) return "Not in database";
    const std::string counts = std::to_string(confidence()) + "/" + std::to_string(totalConfidence);
    if (!accurate()) return "Not accurate (confidence " + counts + ")";
    const char* version = v1Confidence && v2Confidence ? "v1+v2" : v2Confidence ? "v2" : "v1";
    return "Accurately ripped (confidence " + counts + ", " + version + ")";
}

}  // namespace cdr
