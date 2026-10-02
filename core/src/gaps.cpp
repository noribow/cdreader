#include "cdreader/gaps.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "cdreader/subchannel.h"

namespace cdr {

namespace {

// A Q frame claiming a position further than this from the sector it was read
// with (after the Q delay, see below) is not trusted (a frame without CRC that
// happens to look valid).
constexpr int32_t kMaxSkew = 10;
// Sectors per READ CD: neighbours stand in for a sector whose frame is mode
// 2 / 3 (MCN / ISRC) or corrupt, without another seek.
constexpr uint32_t kBlockSectors = 3;
// A probe that found nothing reads this many sectors around its target (and
// then next to them on both sides): with a Q delay the frames of the positions
// wanted come with other sectors, and when only one or two positions are
// still searched the 1..3 sectors of the normal read may hold none of them
// (#41).
constexpr uint32_t kWideSectors = 16;
// A constant Q delay is taken once this many position frames were seen and at
// least 90 % of them agree.
constexpr unsigned kDelaySamples = 8;
// Sectors between the last position seen before a boundary and the first one
// after it that may stay unknown (no frame there carries a position) for the
// boundary to be taken at the later one rather than given up.
constexpr int32_t kMaxUnresolved = 3;

struct BudgetExhausted {};
struct SearchFailed {
    std::string why;
};

struct ReadOutcome {
    bool illegalRequest = false;  // the drive rejected the command
    bool failed = false;
    std::vector<QFrame> frames;   // usable mode 1 frames close to the sectors read
};

struct Observation {
    int32_t lba = 0;
    int key = 0;  // track * 100 + index
};

// A boundary found by Detector::findFirst / searchBoundary.
struct Boundary {
    int32_t at = 0;   // the first position whose key is >= k
    int key = -1;     // its key (-1: not read, the upper end given)
    int32_t below = 0;  // the last position known to be < k (at - 1, unless unresolved)
    int32_t unresolved() const { return at - below - 1; }
};

std::string lbaText(int64_t lba) { return std::to_string(lba); }
std::string signedText(int64_t v) { return v > 0 ? "+" + std::to_string(v) : std::to_string(v); }

std::string sourceName(QSource s) {
    switch (s) {
        case QSource::FormattedQ: return "formatted Q";
        case QSource::RawPW: return "raw P-W";
        case QSource::CurrentPosition: return "READ SUB-CHANNEL";
        case QSource::Auto: break;
    }
    return "automatic";
}

class Detector {
public:
    Detector(CdDrive& drive, DiscGaps& out, unsigned maxReads, bool fallback)
        : drive_(drive), out_(out), maxReads_(maxReads), fallback_(fallback) {}

    QSource source = QSource::FormattedQ;
    bool vote = false;  // positions need two agreeing frames unless the CRC was checked
    // Sectors that may be read as audio (the audio tracks around the search).
    int32_t rangeBegin = 0;
    int32_t rangeEnd = std::numeric_limits<int32_t>::max();

    int32_t delay() const { return delay_; }
    void markUnavailable(QSource s) { unavailable_.insert(s); }
    void countUsed(size_t n) { tally([&](QFrameStats& s) { s.used += unsigned(n); }); }

    // Reads the Q frames of [lba, lba + count) (count 1 for CurrentPosition),
    // clipped to the readable range.
    ReadOutcome read(int32_t lba, uint32_t count) { return readWith(source, lba, count); }

    ReadOutcome readWith(QSource src, int32_t lba, uint32_t count) {
        ReadOutcome o;
        const int64_t begin = std::max<int64_t>(lba, rangeBegin);
        const int64_t end = std::min<int64_t>(int64_t(lba) + (src == QSource::CurrentPosition ? 1 : count), rangeEnd);
        if (end <= begin) return o;
        lba = int32_t(begin);
        count = uint32_t(end - begin);
        auto fail = [&](const ScsiResult& r) {
            o.failed = true;
            o.illegalRequest = r.transportOk && r.status == 0x02 && r.sense.key == 0x5;
            tally([](QFrameStats& s) { ++s.failedCommands; });
            return o;
        };
        if (src == QSource::CurrentPosition) {
            spend(2);
            std::vector<uint8_t> audio(kSectorBytes);
            ScsiResult r = drive_.readAudio(uint32_t(lba), 1, audio.data());
            if (!r.ok()) return fail(r);
            uint8_t d[16] = {};
            r = drive_.readSubChannel(SubChannelFormat::CurrentPosition, 0, d, sizeof d, false);
            if (!r.ok()) return fail(r);
            add(o, parseCurrentPosition(d, std::min(r.transferred, sizeof d)), lba, false);
            return o;
        }
        spend(1);
        const SubChannelSelection selection =
            src == QSource::RawPW ? SubChannelSelection::RawPW : SubChannelSelection::FormattedQ;
        const size_t perSector = kSectorBytes + subChannelBytesPerSector(selection);
        buffer_.assign(size_t(count) * perSector, 0);
        const ScsiResult r = drive_.readAudioWithSubChannel(uint32_t(lba), count, selection, buffer_.data());
        if (!r.ok()) return fail(r);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* q = buffer_.data() + size_t(i) * perSector + kSectorBytes;
            add(o, selection == SubChannelSelection::RawPW ? parseRawPwQ(q) : parseFormattedQ(q), lba + int32_t(i),
                true);
        }
        return o;
    }

    // A trusted observation at a position strictly between lo and hi, as
    // close to `target` as possible. Throws SearchFailed / BudgetExhausted.
    Observation probe(int32_t target, int32_t lo, int32_t hi) {
        const auto cached = crcChecked_.find(target);
        if (cached != crcChecked_.end()) return {target, cached->second};
        QFrameStats stats;
        ProbeScope scope(*this, stats);
        std::map<int32_t, std::vector<int>> votes;
        Observation best;
        // One read of the positions [first, first + count) (READ CD sources:
        // the sectors read are moved by the Q delay); true when it found one.
        auto attempt = [&](QSource src, int32_t first, uint32_t count) {
            const int32_t sector = src == QSource::CurrentPosition ? first : first - delay_;
            const ReadOutcome o = readWith(src, sector, count);
            if (o.illegalRequest) {
                if (src == source)
                    throw SearchFailed{"the drive rejected the command at LBA " + lbaText(std::max(sector, rangeBegin))};
                unavailable_.insert(src);
                return false;
            }
            bool found = false;
            for (const QFrame& f : o.frames) {
                const int32_t p = f.absoluteLba;
                if (p <= lo || p >= hi) {
                    tally([](QFrameStats& s) { ++s.outsideWindow; });
                    continue;
                }
                bool trusted = true;
                if (f.crc == QFrame::Crc::Valid) {
                    crcChecked_[p] = f.key();
                } else if (vote) {
                    std::vector<int>& v = votes[p];
                    v.push_back(f.key());
                    trusted = std::count(v.begin(), v.end(), f.key()) >= 2;
                }
                tally([&](QFrameStats& s) { ++(trusted ? s.used : s.other); });
                if (trusted && (!found || std::abs(p - target) < std::abs(best.lba - target))) {
                    best = {p, f.key()};
                    found = true;
                }
            }
            return found;
        };

        // Where to read: the target block again (a bad frame is usually a
        // one-off), then the blocks next to it. Voting needs more reads.
        static const int kPlain[] = {0, 0, 3, -3};
        static const int kVoting[] = {0, 0, 0, 3, 3, -3, -3};
        const int* shifts = vote ? kVoting : kPlain;
        const size_t attempts = vote ? std::size(kVoting) : std::size(kPlain);
        for (size_t i = 0; i < attempts; ++i) {
            const int32_t center = std::clamp(target + shifts[i], lo + 1, hi - 1);
            int32_t first = center;
            uint32_t n = 1;
            if (source != QSource::CurrentPosition) {
                first = std::max(lo + 1, center - 1);
                n = uint32_t(std::min<int64_t>(kBlockSectors, int64_t(hi) - first));
            }
            if (attempt(source, first, n)) return best;
        }

        // Nothing: when the interval is narrow the reads above were all the
        // same 1..3 sectors. Read wider, then next to that (#41).
        const int32_t half = int32_t(kWideSectors / 2);
        if (source != QSource::CurrentPosition) {
            for (int32_t c : {target, target - int32_t(kWideSectors), target + int32_t(kWideSectors)}) {
                const int32_t first = c - half, last = c + half;  // positions [first, last)
                if (c != target && (last + kMaxSkew <= lo + 1 || first - kMaxSkew >= hi)) continue;
                for (int pass = 0; pass < (vote ? 2 : 1); ++pass)
                    if (attempt(source, first, kWideSectors)) {
                        ++out_.widerProbes;
                        return best;
                    }
            }
        } else {
            for (int32_t step : {1, -1, 2, -2}) {
                const int32_t p = target + step;
                if (p <= lo || p >= hi) continue;
                for (int pass = 0; pass < (vote ? 2 : 1); ++pass)
                    if (attempt(source, p, 1)) {
                        ++out_.widerProbes;
                        return best;
                    }
            }
        }

        // Then the other ways of reading Q frames, for this position only.
        std::string tried;
        if (fallback_) {
            const QSource order[] = {QSource::FormattedQ, QSource::RawPW, QSource::CurrentPosition};
            const QSource* self = std::find(std::begin(order), std::end(order), source);
            for (const QSource* s = self == std::end(order) ? std::end(order) : self + 1; s != std::end(order); ++s) {
                if (unavailable_.count(*s)) continue;
                tried += (tried.empty() ? "; also tried " : ", ") + sourceName(*s);
                if (*s == QSource::RawPW) {
                    // Raw P-W frames always carry a CRC: no voting needed.
                    if (attempt(*s, target - half, kWideSectors)) {
                        ++out_.fallbackProbes[*s];
                        return best;
                    }
                    continue;
                }
                for (int32_t step : {0, 1, -1}) {
                    const int32_t p = target + step;
                    if (p <= lo || p >= hi) continue;
                    for (int pass = 0; pass < (vote ? 2 : 1); ++pass)
                        if (attempt(*s, p, 1)) {
                            ++out_.fallbackProbes[*s];
                            return best;
                        }
                    if (unavailable_.count(*s)) break;
                }
            }
        }
        throw SearchFailed{"no usable Q frame near LBA " + lbaText(target) + " [" + stats.summary() + tried + "]"};
    }

    // The first position in (lo, hi] whose key is >= k, given key(lo) < k and
    // key(hi) >= k (hi itself is not read: it comes from the TOC or an earlier
    // probe). The key is -1 if never read (hi). When no frame with a position
    // can be found for kMaxUnresolved sectors before the first position seen
    // with key >= k, that one is returned (Boundary::unresolved() > 0).
    Boundary findFirst(int k, int32_t lo, int32_t hi, bool gallop) {
        int keyHi = -1;
        const int32_t top = hi;
        bool unresolved = false;
        // probe(), or false when it found nothing and only kMaxUnresolved positions are left.
        auto tryProbe = [&](int32_t target, Observation& o) {
            try {
                o = probe(target, lo, hi);
                return true;
            } catch (const SearchFailed&) {
                if (hi - lo - 1 <= kMaxUnresolved) return false;
                throw;
            }
        };
        if (gallop) {
            for (int64_t d = 1; int64_t(top) - d > lo && hi - lo > 1; d *= 2) {
                const int32_t target = int32_t(std::max<int64_t>(int64_t(top) - d, lo + 1));
                if (target >= hi) continue;
                Observation o;
                if (!tryProbe(target, o)) {
                    unresolved = true;
                    break;
                }
                if (o.key >= k) {
                    hi = o.lba;
                    keyHi = o.key;
                } else {
                    lo = o.lba;
                    break;
                }
            }
        }
        while (!unresolved && hi - lo > 1) {
            Observation o;
            if (!tryProbe(lo + (hi - lo) / 2, o)) break;
            if (o.key >= k) {
                hi = o.lba;
                keyHi = o.key;
            } else {
                lo = o.lba;
            }
        }
        Boundary b;
        b.at = hi;
        b.key = keyHi;
        b.below = lo;
        return b;
    }

    // Reads the neighbours of boundary b again: the last position before it
    // must be < k, b itself >= k (unless b is the known upper end).
    bool confirm(int k, const Boundary& b, int32_t lo, int32_t hi) {
        if (b.below > lo && probe(b.below, std::max(lo, b.below - 2), b.at).key >= k) return false;
        if (b.at != hi && probe(b.at, b.at - 1, std::min(hi + 1, b.at + 2)).key < k) return false;
        return true;
    }

    Boundary searchBoundary(int k, int32_t lo, int32_t hi, bool gallop) {
        for (;;) {
            const Boundary found = findFirst(k, lo, hi, gallop);
            if (confirm(k, found, lo, hi)) {
                out_.unresolvedSectors += unsigned(found.unresolved());
                return found;
            }
            if (vote)
                throw SearchFailed{"inconsistent Q frames around LBA " + lbaText(found.at)};
            vote = true;  // the drive returned a bad frame without CRC: search again, more carefully
        }
    }

private:
    // Counts the frames of one probe as well as those of the disc.
    class ProbeScope {
    public:
        ProbeScope(Detector& d, QFrameStats& s) : d_(d), saved_(d.probe_) { d.probe_ = &s; }
        ~ProbeScope() { d_.probe_ = saved_; }
        ProbeScope(const ProbeScope&) = delete;
        ProbeScope& operator=(const ProbeScope&) = delete;

    private:
        Detector& d_;
        QFrameStats* saved_;
    };

    template <class F>
    void tally(F f) {
        f(out_.qFrames);
        if (probe_) f(*probe_);
    }

    void spend(unsigned commands) {
        if (out_.reads + commands > maxReads_) throw BudgetExhausted{};
        out_.reads += commands;
    }

    // Classifies a frame read with `sector`; keeps it when it is usable.
    void add(ReadOutcome& o, const QFrame& f, int32_t sector, bool readCd) {
        tally([](QFrameStats& s) { ++s.frames; });
        if (f.crc == QFrame::Crc::Invalid) return tally([](QFrameStats& s) { ++s.crcErrors; });
        if (f.adr == 2 || f.adr == 3) return tally([](QFrameStats& s) { ++s.otherAdr; });
        if (f.adr != 1) return tally([](QFrameStats& s) { ++s.other; });
        if (!f.parsed) return tally([](QFrameStats& s) { ++s.badBcd; });
        const int32_t delta = f.absoluteLba - sector;
        tally([&](QFrameStats& s) { ++s.deltas[delta]; });
        if (readCd && std::abs(delta) <= kMaxSkew) noteDelay(delta);
        if (std::abs(f.absoluteLba - (sector + (readCd ? delay_ : 0))) > kMaxSkew)
            return tally([](QFrameStats& s) { ++s.farOff; });
        o.frames.push_back(f);
    }

    // The Q delay: the delta nearly all position frames read with READ CD agree on.
    void noteDelay(int32_t delta) {
        ++delayVotes_[delta];
        ++delaySamples_;
        if (delaySamples_ < kDelaySamples) return;
        const auto mode = std::max_element(delayVotes_.begin(), delayVotes_.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
        delay_ = uint64_t(mode->second) * 10 >= uint64_t(delaySamples_) * 9 ? mode->first : 0;
        out_.qDelay = delay_;
    }

    CdDrive& drive_;
    DiscGaps& out_;
    unsigned maxReads_;
    bool fallback_;  // other Q sources may answer a probe the chosen one cannot
    std::vector<uint8_t> buffer_;
    std::map<int32_t, int> crcChecked_;  // keys of frames whose CRC was valid, by position
    std::set<QSource> unavailable_;      // sources the drive rejected
    QFrameStats* probe_ = nullptr;       // frames of the current probe
    std::map<int32_t, unsigned> delayVotes_;
    unsigned delaySamples_ = 0;
    int32_t delay_ = 0;
};

// Picks the first method that returns usable frames near the start of the
// first audio track. Returns false (and sets `out`) when none does.
bool chooseSource(Detector& d, const Toc& toc, const GapDetectionOptions& options, DiscGaps& out) {
    const Track* first = nullptr;
    for (const Track& t : toc.tracks)
        if (t.isAudio && t.lengthSectors > 0) {
            first = &t;
            break;
        }
    if (first == nullptr) {
        out.status = DiscGaps::Status::Unsupported;
        out.detail = "no audio track";
        return false;
    }
    std::vector<QSource> candidates = {options.source};
    if (options.source == QSource::Auto)
        candidates = {QSource::FormattedQ, QSource::RawPW, QSource::CurrentPosition};
    bool allRejected = true;
    for (QSource c : candidates) {
        d.source = c;
        bool rejected = false;
        for (uint32_t attempt = 0; attempt < 3; ++attempt) {
            const uint32_t lba = first->startLba + std::min(first->lengthSectors - 1, attempt * 7);
            const uint32_t count = c == QSource::CurrentPosition
                                       ? 1
                                       : std::min(kBlockSectors, first->endLba() - lba);
            const ReadOutcome o = d.read(int32_t(lba), count);
            if (o.illegalRequest) {
                rejected = true;
                d.markUnavailable(c);
                break;
            }
            d.countUsed(o.frames.size());
            if (!o.frames.empty()) {
                out.source = c;
                return true;
            }
        }
        allRejected = allRejected && rejected;
    }
    out.status = DiscGaps::Status::Unsupported;
    out.detail = allRejected ? "the drive does not return Q sub-channel data" : "no usable Q sub-channel frames";
    return false;
}

// "INDEX 00 may start 1 sector earlier: no position in the Q frame at LBA n".
std::string unresolvedText(int index, const Boundary& b) {
    const int32_t n = b.unresolved();
    char buf[64];
    std::snprintf(buf, sizeof buf, "INDEX %02d may start %d sector%s earlier: ", index, int(n), n == 1 ? "" : "s");
    return buf + (n == 1 ? "no position in the Q frame at LBA " + lbaText(b.below + 1)
                         : "no position in the Q frames at LBA " + lbaText(b.below + 1) + "-" + lbaText(b.at - 1));
}

std::string pregapText(const TrackIndexes& t, bool htoa) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "Track %2d  ", t.track);
    std::string s = buf;
    switch (t.status) {
        case TrackIndexes::Status::Detected:
            s += "pregap " + formatMsf(t.pregapSectors);
            if (t.pregapSectors) s += "  INDEX 00 at LBA " + std::to_string(t.index00Lba());
            if (htoa) s += " (HTOA)";
            for (size_t i = 0; i < t.laterIndexes.size(); ++i) {
                std::snprintf(buf, sizeof buf, "  INDEX %02d at LBA %u", int(i + 2), t.laterIndexes[i]);
                s += buf;
            }
            if (!t.detail.empty()) s += "  (" + t.detail + ")";
            break;
        case TrackIndexes::Status::Unknown:
            s += "pregap unknown (" + t.detail + ")";
            break;
        case TrackIndexes::Status::Skipped:
            s += "pregap not searched (" + t.detail + ")";
            break;
    }
    return s;
}

}  // namespace

std::string describeQSource(QSource source) {
    switch (source) {
        case QSource::Auto: return "automatic";
        case QSource::FormattedQ: return "READ CD with formatted Q sub-channel";
        case QSource::RawPW: return "READ CD with raw P-W sub-channel";
        case QSource::CurrentPosition: return "READ SUB-CHANNEL current position";
    }
    return {};
}

void QFrameStats::add(const QFrameStats& o) {
    frames += o.frames;
    used += o.used;
    otherAdr += o.otherAdr;
    crcErrors += o.crcErrors;
    badBcd += o.badBcd;
    farOff += o.farOff;
    outsideWindow += o.outsideWindow;
    other += o.other;
    failedCommands += o.failedCommands;
    for (const auto& [delta, n] : o.deltas) deltas[delta] += n;
}

namespace {

// "6 ADR 2/3, 2 CRC" (only the reasons that occurred).
std::string rejectionText(const QFrameStats& s) {
    std::string t;
    auto item = [&](unsigned n, const char* what) {
        if (n == 0) return;
        if (!t.empty()) t += ", ";
        t += std::to_string(n) + " " + what;
    };
    item(s.otherAdr, "ADR 2/3");
    item(s.crcErrors, "CRC");
    item(s.badBcd, "bad BCD");
    item(s.farOff, "too far from the sector");
    item(s.outsideWindow, "out of window");
    item(s.other, "other");
    return t;
}

}  // namespace

std::string QFrameStats::summary() const {
    std::string t = std::to_string(frames) + " Q frame" + (frames == 1 ? "" : "s");
    const std::string rejections = rejectionText(*this);
    if (!rejections.empty()) t += ": " + rejections;
    if (!deltas.empty()) {
        const int32_t lo = deltas.begin()->first, hi = deltas.rbegin()->first;
        t += ", position delta " + signedText(lo) + (lo == hi ? "" : ".." + signedText(hi));
    }
    if (failedCommands) t += ", " + std::to_string(failedCommands) + " failed command" + (failedCommands == 1 ? "" : "s");
    return t;
}

std::string QFrameStats::logLine() const {
    std::string t = "Q frames: " + std::to_string(frames) + " read, " + std::to_string(used) + " used; rejected: ";
    const std::string rejections = rejectionText(*this);
    t += rejections.empty() ? "none" : rejections;
    if (!deltas.empty()) {
        unsigned total = 0;
        for (const auto& d : deltas) total += d.second;
        int32_t median = 0;
        unsigned seen = 0;
        for (const auto& [delta, n] : deltas) {
            seen += n;
            if (seen > (total - 1) / 2) {
                median = delta;
                break;
            }
        }
        t += "; position delta min " + signedText(deltas.begin()->first) + " / max " +
             signedText(deltas.rbegin()->first) + " / median " + signedText(median);
    }
    if (failedCommands)
        t += "; " + std::to_string(failedCommands) + " failed command" + (failedCommands == 1 ? "" : "s");
    return t;
}

const TrackIndexes* DiscGaps::find(int track) const {
    for (const TrackIndexes& t : tracks)
        if (t.track == track) return &t;
    return nullptr;
}

uint32_t DiscGaps::pregap(int track) const {
    if (track == 1 && htoaSectors > 0) return htoaSectors;
    const TrackIndexes* t = find(track);
    return t && t->detected() ? t->pregapSectors : 0;
}

std::vector<uint32_t> DiscGaps::laterIndexes(int track) const {
    const TrackIndexes* t = find(track);
    return t && t->detected() ? t->laterIndexes : std::vector<uint32_t>();
}

std::vector<std::string> DiscGaps::logLines() const {
    std::vector<std::string> lines;
    switch (status) {
        case Status::NotRun:
            lines.push_back("Gap detection: not run (disabled)");
            break;
        case Status::Unsupported:
            lines.push_back("Gap detection: not possible (" + detail + "), no pregaps known");
            break;
        case Status::Detected:
        case Status::Partial: {
            char buf[160];
            std::snprintf(buf, sizeof buf, "Gap detection: %s, %u reads in %.1f s%s", describeQSource(source).c_str(),
                          reads, seconds, status == Status::Partial ? ", some tracks unknown" : "");
            lines.push_back(buf);
            break;
        }
    }
    if (hasHtoa()) {
        const bool searched = status == Status::Detected || status == Status::Partial;
        lines.push_back("HTOA (hidden track before track 1): " + formatMsf(htoaSectors) + ", LBA 0-" +
                        std::to_string(htoaSectors - 1) +
                        (!searched        ? " (from the TOC)"
                         : htoaConfirmed ? ", confirmed by the Q sub-channel"
                                         : ", not confirmed by the Q sub-channel"));
    }
    for (const TrackIndexes& t : tracks) lines.push_back(pregapText(t, t.track == 1 && hasHtoa()));
    if ((status == Status::Detected || status == Status::Partial) && (qFrames.frames || qFrames.failedCommands)) {
        lines.push_back(qFrames.logLine());
        if (qDelay != 0)
            lines.push_back("Q delay: " + signedText(qDelay) + " sector" + (std::abs(qDelay) == 1 ? "" : "s") +
                            " (the Q frame read with sector n is that of n " + (qDelay > 0 ? "+ " : "- ") +
                            std::to_string(std::abs(qDelay)) + "; reads moved to make up for it)");
        std::string retries;
        if (widerProbes)
            retries = std::to_string(widerProbes) + " position" + (widerProbes == 1 ? "" : "s") + " found with a wider read";
        for (const auto& [source, n] : fallbackProbes) {
            if (!retries.empty()) retries += ", ";
            retries += std::to_string(n) + " with " + describeQSource(source);
        }
        if (!retries.empty()) lines.push_back("Q retries: " + retries);
    }
    return lines;
}

uint32_t htoaSectors(const Toc& toc) {
    const Track* first = toc.findTrack(1);
    return first && first->isAudio && toc.tracks.front().number == 1 ? first->startLba : 0;
}

Track htoaTrack(const Toc& toc) {
    Track t;
    t.number = 0;
    t.startLba = 0;
    t.lengthSectors = htoaSectors(toc);
    if (const Track* first = toc.findTrack(1)) {
        t.preEmphasis = first->preEmphasis;
        t.copyPermitted = first->copyPermitted;
    }
    return t;
}

DiscGaps gapsFromToc(const Toc& toc) {
    DiscGaps gaps;
    gaps.htoaSectors = htoaSectors(toc);
    return gaps;
}

DiscGaps detectGaps(CdDrive& drive, const Toc& toc, const GapDetectionOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    DiscGaps out = gapsFromToc(toc);
    Detector d(drive, out, options.maxReads, options.source == QSource::Auto);
    auto finish = [&] {
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return out;
    };

    // Reads stay inside the audio tracks around the one searched.
    auto readable = [&](const Track& t) {
        const Toc::LbaRange r = toc.audioRange(t);
        d.rangeBegin = int32_t(r.begin);
        d.rangeEnd = int32_t(r.end);
    };
    for (const Track& t : toc.tracks)
        if (t.isAudio) {
            readable(t);
            break;
        }
    try {
        if (!chooseSource(d, toc, options, out)) return finish();
    } catch (const BudgetExhausted&) {
        out.status = DiscGaps::Status::Unsupported;
        out.detail = "read budget exhausted";
        return finish();
    } catch (const std::exception& e) {
        out.status = DiscGaps::Status::Unsupported;
        out.detail = e.what();
        return finish();
    }

    bool exhausted = false;
    for (size_t i = 0; i < toc.tracks.size(); ++i) {
        const Track& t = toc.tracks[i];
        if (!t.isAudio) continue;
        TrackIndexes ti;
        ti.track = t.number;
        ti.index01Lba = t.startLba;
        if (options.progress) options.progress(t.number, toc.lastTrack);
        readable(t);
        try {
            if (exhausted) throw BudgetExhausted{};
            if (i == 0) {
                // The first track: only an HTOA can precede it in the readable area.
                ti.status = TrackIndexes::Status::Detected;
                ti.pregapSectors = t.number == 1 ? out.htoaSectors : 0;
                if (ti.pregapSectors) {
                    const int32_t start = int32_t(t.startLba);
                    const int k = t.number * 100;  // INDEX 00 of track 1
                    out.htoaConfirmed = d.probe(0, -1, start).key == k && d.probe(start - 1, -1, start).key == k;
                    if (!out.htoaConfirmed) ti.detail = "the Q sub-channel does not show INDEX 00 before track 1";
                }
            } else {
                const Track& prev = toc.tracks[i - 1];
                if (!prev.isAudio) {
                    ti.status = TrackIndexes::Status::Skipped;
                    ti.detail = "follows a data track";
                } else if (prev.endLba() != t.startLba) {
                    ti.status = TrackIndexes::Status::Skipped;
                    ti.detail = "not adjacent to the previous track";
                } else {
                    const Boundary found =
                        d.searchBoundary(t.number * 100, int32_t(prev.startLba), int32_t(t.startLba), true);
                    ti.pregapSectors = t.startLba - uint32_t(found.at);
                    ti.status = TrackIndexes::Status::Detected;
                    if (found.unresolved() > 0) ti.detail = unresolvedText(0, found);
                }
            }
        } catch (const BudgetExhausted&) {
            exhausted = true;
            ti.status = TrackIndexes::Status::Unknown;
            ti.detail = "read budget exhausted";
        } catch (const SearchFailed& e) {
            if (ti.status != TrackIndexes::Status::Detected) ti.status = TrackIndexes::Status::Unknown;
            ti.detail = e.why;
        } catch (const std::exception& e) {
            if (ti.status != TrackIndexes::Status::Detected) ti.status = TrackIndexes::Status::Unknown;
            ti.detail = e.what();
        }
        out.tracks.push_back(ti);
    }

    // INDEX 02+: one read at the end of each track (before the next pregap)
    // shows whether there are any.
    if (options.laterIndexes && !exhausted) {
        for (size_t i = 0; i < out.tracks.size(); ++i) {
            TrackIndexes& ti = out.tracks[i];
            const Track* t = toc.findTrack(ti.track);
            if (!ti.detected() || t == nullptr) continue;
            uint32_t end = t->endLba();
            if (i + 1 < out.tracks.size() && out.tracks[i + 1].detected() &&
                out.tracks[i + 1].index01Lba == t->endLba())
                end = out.tracks[i + 1].index00Lba();
            if (end < t->startLba + 3) continue;
            readable(*t);
            // Notes of the pregap search stay; those of this scan are added.
            auto note = [&](const std::string& text) { ti.detail += (ti.detail.empty() ? "" : "; ") + text; };
            try {
                int32_t lo = int32_t(t->startLba);
                const Observation last = d.probe(int32_t(end) - 1, lo, int32_t(end));
                if (last.key / 100 != ti.track) {
                    // The end of the track is the pregap of the next one, which
                    // could not be found: searching would only repeat that search.
                    note("INDEX 02+: not searched (the pregap of track " + std::to_string(last.key / 100) +
                         " is unknown)");
                    continue;
                }
                for (int j = 2; j <= 99; ++j) {
                    const int k = ti.track * 100 + j;
                    if (last.key < k) break;
                    Boundary found = d.searchBoundary(k, lo, last.lba, false);
                    if (found.key < 0) found.key = last.key;
                    if (found.key / 100 != ti.track) break;  // reached the next track
                    if (found.key != k) {
                        note("index numbers skip from " + std::to_string(j - 1));
                        break;
                    }
                    if (found.unresolved() > 0) note(unresolvedText(j, found));
                    ti.laterIndexes.push_back(uint32_t(found.at));
                    lo = found.at;
                }
            } catch (const BudgetExhausted&) {
                note("INDEX 02+: read budget exhausted");
                exhausted = true;
                break;
            } catch (const SearchFailed& e) {
                note("INDEX 02+: " + e.why);
            } catch (const std::exception& e) {
                note(std::string("INDEX 02+: ") + e.what());
            }
        }
    }

    out.status = DiscGaps::Status::Detected;
    for (const TrackIndexes& ti : out.tracks)
        if (ti.status == TrackIndexes::Status::Unknown) out.status = DiscGaps::Status::Partial;
    return finish();
}

}  // namespace cdr
