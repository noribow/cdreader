#include "cdreader/gaps.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <iterator>
#include <map>
#include <utility>

#include "cdreader/subchannel.h"

namespace cdr {

namespace {

// A Q frame claiming a position further than this from the sector it was read
// with is not trusted (a frame without CRC that happens to look valid).
constexpr int32_t kMaxSkew = 10;
// Sectors per READ CD: neighbours stand in for a sector whose frame is mode
// 2 / 3 (MCN / ISRC) or corrupt, without another seek.
constexpr uint32_t kBlockSectors = 3;

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

std::string lbaText(int64_t lba) { return std::to_string(lba); }

class Detector {
public:
    Detector(CdDrive& drive, DiscGaps& out, unsigned maxReads) : drive_(drive), out_(out), maxReads_(maxReads) {}

    QSource source = QSource::FormattedQ;
    bool vote = false;  // positions need two agreeing frames unless the CRC was checked

    // Reads the Q frames of [lba, lba + count) (count 1 for CurrentPosition).
    ReadOutcome read(int32_t lba, uint32_t count) {
        ReadOutcome o;
        auto fail = [&](const ScsiResult& r) {
            o.failed = true;
            o.illegalRequest = r.transportOk && r.status == 0x02 && r.sense.key == 0x5;
            return o;
        };
        if (source == QSource::CurrentPosition) {
            spend(2);
            std::vector<uint8_t> audio(kSectorBytes);
            ScsiResult r = drive_.readAudio(uint32_t(lba), 1, audio.data());
            if (!r.ok()) return fail(r);
            uint8_t d[16] = {};
            r = drive_.readSubChannel(SubChannelFormat::CurrentPosition, 0, d, sizeof d, false);
            if (!r.ok()) return fail(r);
            add(o, parseCurrentPosition(d, std::min(r.transferred, sizeof d)), lba);
            return o;
        }
        spend(1);
        const SubChannelSelection selection =
            source == QSource::RawPW ? SubChannelSelection::RawPW : SubChannelSelection::FormattedQ;
        const size_t perSector = kSectorBytes + subChannelBytesPerSector(selection);
        buffer_.assign(size_t(count) * perSector, 0);
        const ScsiResult r = drive_.readAudioWithSubChannel(uint32_t(lba), count, selection, buffer_.data());
        if (!r.ok()) return fail(r);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* q = buffer_.data() + size_t(i) * perSector + kSectorBytes;
            add(o, selection == SubChannelSelection::RawPW ? parseRawPwQ(q) : parseFormattedQ(q), lba + int32_t(i));
        }
        return o;
    }

    // A trusted observation at a position strictly between lo and hi, as
    // close to `target` as possible. Throws SearchFailed / BudgetExhausted.
    Observation probe(int32_t target, int32_t lo, int32_t hi) {
        const auto cached = crcChecked_.find(target);
        if (cached != crcChecked_.end()) return {target, cached->second};
        // Where to read: the target block again (a bad frame is usually a
        // one-off), then the blocks next to it. Voting needs more reads.
        static const int kPlain[] = {0, 0, 3, -3};
        static const int kVoting[] = {0, 0, 0, 3, 3, -3, -3};
        const int* shifts = vote ? kVoting : kPlain;
        const size_t attempts = vote ? std::size(kVoting) : std::size(kPlain);
        std::map<int32_t, std::vector<int>> votes;
        for (size_t attempt = 0; attempt < attempts; ++attempt) {
            const int32_t center = std::clamp(target + shifts[attempt], lo + 1, hi - 1);
            int32_t start = center;
            uint32_t count = 1;
            if (source != QSource::CurrentPosition) {
                start = std::max(lo + 1, center - 1);
                count = uint32_t(std::min<int64_t>(kBlockSectors, int64_t(hi) - start));
            }
            const ReadOutcome o = read(start, count);
            if (o.illegalRequest) throw SearchFailed{"the drive rejected the command at LBA " + lbaText(start)};
            bool found = false;
            Observation best;
            for (const QFrame& f : o.frames) {
                const int32_t p = f.absoluteLba;
                if (p <= lo || p >= hi) continue;
                bool trusted = true;
                if (f.crc == QFrame::Crc::Valid) {
                    crcChecked_[p] = f.key();
                } else if (vote) {
                    std::vector<int>& v = votes[p];
                    v.push_back(f.key());
                    trusted = std::count(v.begin(), v.end(), f.key()) >= 2;
                }
                if (trusted && (!found || std::abs(p - target) < std::abs(best.lba - target))) {
                    best = {p, f.key()};
                    found = true;
                }
            }
            if (found) return best;
        }
        throw SearchFailed{"no usable Q frame near LBA " + lbaText(target)};
    }

    // The first position in (lo, hi] whose key is >= k, given key(lo) < k and
    // key(hi) >= k (hi itself is not read: it comes from the TOC or an earlier
    // probe). Returns the position and its key (-1 if never read: hi).
    std::pair<int32_t, int> findFirst(int k, int32_t lo, int32_t hi, bool gallop) {
        int keyHi = -1;
        const int32_t top = hi;
        if (gallop) {
            for (int64_t d = 1; int64_t(top) - d > lo && hi - lo > 1; d *= 2) {
                const int32_t target = int32_t(std::max<int64_t>(int64_t(top) - d, lo + 1));
                if (target >= hi) continue;
                const Observation o = probe(target, lo, hi);
                if (o.key >= k) {
                    hi = o.lba;
                    keyHi = o.key;
                } else {
                    lo = o.lba;
                    break;
                }
            }
        }
        while (hi - lo > 1) {
            const Observation o = probe(lo + (hi - lo) / 2, lo, hi);
            if (o.key >= k) {
                hi = o.lba;
                keyHi = o.key;
            } else {
                lo = o.lba;
            }
        }
        return {hi, keyHi};
    }

    // Reads the neighbours of boundary b again: the sector before must be < k,
    // b itself >= k (unless b is the known upper end).
    bool confirm(int k, int32_t b, int32_t lo, int32_t hi) {
        if (b - 1 > lo && probe(b - 1, std::max(lo, b - 3), b).key >= k) return false;
        if (b != hi && probe(b, b - 1, std::min(hi + 1, b + 2)).key < k) return false;
        return true;
    }

    std::pair<int32_t, int> searchBoundary(int k, int32_t lo, int32_t hi, bool gallop) {
        for (;;) {
            const std::pair<int32_t, int> found = findFirst(k, lo, hi, gallop);
            if (confirm(k, found.first, lo, hi)) return found;
            if (vote)
                throw SearchFailed{"inconsistent Q frames around LBA " + lbaText(found.first)};
            vote = true;  // the drive returned a bad frame without CRC: search again, more carefully
        }
    }

private:
    void spend(unsigned commands) {
        if (out_.reads + commands > maxReads_) throw BudgetExhausted{};
        out_.reads += commands;
    }

    static void add(ReadOutcome& o, const QFrame& f, int32_t expected) {
        if (f.usable() && std::abs(f.absoluteLba - expected) <= kMaxSkew) o.frames.push_back(f);
    }

    CdDrive& drive_;
    DiscGaps& out_;
    unsigned maxReads_;
    std::vector<uint8_t> buffer_;
    std::map<int32_t, int> crcChecked_;  // keys of frames whose CRC was valid, by position
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
                break;
            }
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
    Detector d(drive, out, options.maxReads);
    auto finish = [&] {
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return out;
    };

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
                    const auto found =
                        d.searchBoundary(t.number * 100, int32_t(prev.startLba), int32_t(t.startLba), true);
                    ti.pregapSectors = t.startLba - uint32_t(found.first);
                    ti.status = TrackIndexes::Status::Detected;
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
            try {
                int32_t lo = int32_t(t->startLba);
                const Observation last = d.probe(int32_t(end) - 1, lo, int32_t(end));
                for (int j = 2; j <= 99; ++j) {
                    const int k = ti.track * 100 + j;
                    if (last.key < k) break;
                    std::pair<int32_t, int> found = d.searchBoundary(k, lo, last.lba, false);
                    if (found.second < 0) found.second = last.key;
                    if (found.second / 100 != ti.track) break;  // reached the next track
                    if (found.second != k) {
                        ti.detail = "index numbers skip from " + std::to_string(j - 1);
                        break;
                    }
                    ti.laterIndexes.push_back(uint32_t(found.first));
                    lo = found.first;
                }
            } catch (const BudgetExhausted&) {
                ti.detail = "INDEX 02+: read budget exhausted";
                exhausted = true;
                break;
            } catch (const SearchFailed& e) {
                ti.detail = "INDEX 02+: " + e.why;
            } catch (const std::exception& e) {
                ti.detail = std::string("INDEX 02+: ") + e.what();
            }
        }
    }

    out.status = DiscGaps::Status::Detected;
    for (const TrackIndexes& ti : out.tracks)
        if (ti.status == TrackIndexes::Status::Unknown) out.status = DiscGaps::Status::Partial;
    return finish();
}

}  // namespace cdr
