#include "cdreader/offset_detect.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>
#include <sstream>

namespace cdr {

namespace {

// Unwinds a scan after options.cancelled() or a throwing progress callback.
struct StopScan {};

std::string signedNumber(int v) { return (v > 0 ? "+" : "") + std::to_string(v); }

std::string versionName(int v1, int v2) {
    if (v1 && v2) return "v1+v2";
    if (v2) return "v2";
    if (v1) return "v1";
    return "";
}

// A match of `t` at `offset`, or null.
const AccurateRipOffsetMatch* matchAt(const OffsetDetectTrack& t, int offset) {
    for (const AccurateRipOffsetMatch& m : t.matches)
        if (m.offset == offset) return &m;
    return nullptr;
}

std::string clean(std::string s) {
    for (char& c : s)
        if (c == '|' || c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string cleanNote(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

// Decides the status from the tracks read so far. Returns true when the result is settled (Detected, or a
// conflict that more tracks cannot resolve).
bool evaluate(OffsetDetection& d, const OffsetDetectOptions& options) {
    const int need = d.usableTracks >= options.minAgreeingTracks ? options.minAgreeingTracks
                                                                 : std::max(1, d.usableTracks);
    // Tracks per offset, and their confidence.
    std::map<int, int> tracksAt;
    std::map<int, int> confidenceAt;
    int matchedTracks = 0;
    for (const OffsetDetectTrack& t : d.tracks) {
        matchedTracks += t.matches.empty() ? 0 : 1;
        for (const AccurateRipOffsetMatch& m : t.matches) {
            ++tracksAt[m.offset];
            confidenceAt[m.offset] += m.confidence();
        }
    }
    d.candidates.clear();
    d.agreeingTracks = 0;
    d.v1Confidence = d.v2Confidence = 0;
    d.singleTrack = false;
    if (tracksAt.empty()) {
        d.status = OffsetDetection::Status::NoMatch;
        return false;
    }
    int best = 0;
    for (const auto& [offset, n] : tracksAt) best = std::max(best, n);
    std::vector<int> top;
    for (const auto& [offset, n] : tracksAt)
        if (n == best) top.push_back(offset);
    // The best candidate (for the report): most tracks, then most submissions.
    int offset = top.front();
    for (int o : top)
        if (confidenceAt[o] > confidenceAt[offset]) offset = o;
    d.offset = offset;
    d.agreeingTracks = best;
    for (const OffsetDetectTrack& t : d.tracks) {
        if (const AccurateRipOffsetMatch* m = matchAt(t, offset)) {
            d.v1Confidence += m->v1Confidence;
            d.v2Confidence += m->v2Confidence;
        }
    }
    // Tracks that matched, but not at the best offset, disagree.
    const bool disagreement = matchedTracks > best;
    if (top.size() > 1 || disagreement) {
        d.status = OffsetDetection::Status::Conflict;
        d.candidates.clear();
        for (const auto& [o, n] : tracksAt) d.candidates.push_back(o);
        std::stable_sort(d.candidates.begin(), d.candidates.end(),
                         [&](int a, int b) { return tracksAt.at(a) > tracksAt.at(b); });
        // Another track cannot undo a disagreement, but may break a tie
        // (a track matching two pressings shifted against each other).
        return disagreement;
    }
    if (best >= need) {
        if (need == 1 && d.usableTracks <= 1 && d.confidence() < options.singleTrackMinConfidence) {
            d.status = OffsetDetection::Status::NotEnough;
            return false;
        }
        d.status = OffsetDetection::Status::Detected;
        d.singleTrack = need == 1;
        return true;
    }
    d.status = OffsetDetection::Status::NotEnough;
    return false;
}

}  // namespace

std::string OffsetDetection::matchedVersion() const { return versionName(v1Confidence, v2Confidence); }

std::string OffsetDetection::agreement() const {
    std::string s = std::to_string(agreeingTracks) + " of " + std::to_string(testedTracks()) + " track" +
                    (testedTracks() == 1 ? "" : "s") + " agreed";
    if (!matchedVersion().empty()) s += ", " + matchedVersion();
    if (singleTrack) s += ", only one track in database";
    return s;
}

std::string OffsetDetection::summary() const {
    switch (status) {
        case Status::Detected:
            return "Read offset " + signedNumber(offset) + " (" + agreement() + ", confidence " +
                   std::to_string(confidence()) + ")";
        case Status::NotInDatabase:
            return "This disc is not in the AccurateRip database: the offset cannot be detected with it. "
                   "Try a more popular CD.";
        case Status::LookupFailed:
            return "AccurateRip lookup failed: " + error;
        case Status::NoUsableTracks:
            return "No audio track of this disc has AccurateRip data.";
        case Status::NoMatch:
            return "No offset in -" + std::to_string(maxOffset) + "..+" + std::to_string(maxOffset) +
                   " matches the database (" + std::to_string(testedTracks()) + " track(s) read" +
                   (error.empty() ? "" : "; " + error) + "). Try another disc.";
        case Status::NotEnough:
            if (agreeingTracks == 1 && usableTracks <= 1)
                return "Offset " + signedNumber(offset) + " matches the disc's only track in the database, but with "
                       "too few submissions (confidence " + std::to_string(confidence()) + ") to be sure. Try "
                       "another disc.";
            return "Offset " + signedNumber(offset) + " matches only " + std::to_string(agreeingTracks) + " of " +
                   std::to_string(testedTracks()) + " track(s); not enough to be sure. Try another disc.";
        case Status::Conflict: {
            std::string list;
            for (int o : candidates) list += (list.empty() ? "" : ", ") + signedNumber(o);
            return "The tracks match at different offsets (" + list +
                   "), e.g. pressings shifted against each other. Try another disc.";
        }
        case Status::Cancelled:
            return "Offset detection cancelled.";
    }
    return {};
}

std::vector<std::string> OffsetDetection::logLines() const {
    std::vector<std::string> lines;
    lines.push_back("Read offset detection (AccurateRip disc id " + id.toString() + ", " + std::to_string(pressings) +
                    " pressing(s), offsets -" + std::to_string(maxOffset) + "..+" + std::to_string(maxOffset) + ")");
    for (const OffsetDetectTrack& t : tracks) {
        char buf[160];
        std::string found;
        for (size_t i = 0; i < t.matches.size() && i < 5; ++i) {
            const AccurateRipOffsetMatch& m = t.matches[i];
            std::snprintf(buf, sizeof buf, "%s%s %s (confidence %d)", found.empty() ? "" : ", ",
                          signedNumber(m.offset).c_str(), m.matchedVersion().c_str(), m.confidence());
            found += buf;
        }
        if (t.matches.size() > 5) found += ", ...";
        std::snprintf(buf, sizeof buf, "  Track %02d (%d submissions): ", t.track, t.totalConfidence);
        lines.push_back(buf + (found.empty() ? std::string("no match") : found));
    }
    lines.push_back("  Result: " + summary());
    return lines;
}

std::vector<const Track*> chooseOffsetTracks(const Toc& toc, const std::vector<AccurateRipPressing>& pressings,
                                             const OffsetDetectOptions& options) {
    auto confidence = [&](const Track& t) {
        return matchAccurateRip(pressings, accurateRipEntryIndex(toc, t), t.number, 0, 0).totalConfidence;
    };
    std::vector<const Track*> chosen;
    if (!options.tracks.empty()) {
        for (int n : options.tracks) {
            const Track* t = toc.findTrack(n);
            if (t && t->isAudio && confidence(*t) > 0) chosen.push_back(t);
        }
        return chosen;
    }
    struct Candidate {
        const Track* track;
        int tier;
        int confidence;
    };
    std::vector<Candidate> candidates;
    const size_t audio = toc.audioTrackCount();
    size_t index = 0;
    for (const Track& t : toc.tracks) {
        if (!t.isAudio) continue;
        ++index;
        const int c = confidence(t);
        if (c <= 0) continue;
        const bool edge = audio > 2 && (index == 1 || index == audio);
        const bool length = t.lengthSectors >= options.minTrackSectors && t.lengthSectors <= options.maxTrackSectors;
        candidates.push_back({&t, (length ? 0 : 2) + (edge ? 1 : 0), c});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.track->lengthSectors < b.track->lengthSectors;  // faster
    });
    for (const Candidate& c : candidates) chosen.push_back(c.track);
    return chosen;
}

OffsetDetection detectReadOffset(CdDrive& drive, const Toc& toc, const std::vector<AccurateRipPressing>& pressings,
                                 const OffsetDetectOptions& options) {
    OffsetDetection d;
    d.id = AccurateRipDiscId::fromToc(toc);
    d.maxOffset = options.maxOffset;
    d.pressings = pressings.size();
    const std::vector<const Track*> usable = chooseOffsetTracks(toc, pressings, options);
    d.usableTracks = int(usable.size());
    if (usable.empty()) {
        d.status = OffsetDetection::Status::NoUsableTracks;
        return d;
    }
    const int steps = std::min<int>(int(usable.size()), std::max(1, options.maxTracks));
    std::exception_ptr callbackError;
    for (int step = 0; step < steps; ++step) {
        const Track& track = *usable[size_t(step)];
        if (options.cancelled && options.cancelled()) {
            d.status = OffsetDetection::Status::Cancelled;
            return d;
        }
        OffsetDetectTrack result;
        result.track = track.number;
        const size_t entry = accurateRipEntryIndex(toc, track);
        result.totalConfidence = matchAccurateRip(pressings, entry, track.number, 0, 0).totalConfidence;
        try {
            const AccurateRipOffsetScan scan = scanReadOffsets(
                drive, toc, track, options.maxOffset, options.rip, [&](uint32_t done, uint32_t total) {
                    if (options.cancelled && options.cancelled()) throw StopScan{};
                    if (!options.progress) return;
                    try {
                        options.progress({step + 1, steps, track.number, done, total});
                    } catch (...) {
                        callbackError = std::current_exception();
                        throw StopScan{};
                    }
                });
            result.matches = findAccurateRipOffsets(scan, pressings, entry);
        } catch (const StopScan&) {
            if (callbackError) std::rethrow_exception(callbackError);
            d.status = OffsetDetection::Status::Cancelled;
            return d;
        } catch (const std::exception& e) {
            // A track that cannot be read only counts as "no match".
            d.error = "track " + std::to_string(track.number) + ": " + e.what();
        }
        d.tracks.push_back(std::move(result));
        if (evaluate(d, options)) break;
    }
    return d;
}

OffsetDetection detectReadOffset(CdDrive& drive, const Toc& toc, HttpClient& http, const OffsetDetectOptions& options) {
    const AccurateRipDiscId id = AccurateRipDiscId::fromToc(toc);
    const AccurateRipLookup lookup = lookupAccurateRip(http, id);
    if (lookup.status != AccurateRipLookup::Status::Found) {
        OffsetDetection d;
        d.id = id;
        d.maxOffset = options.maxOffset;
        d.status = lookup.status == AccurateRipLookup::Status::NotFound ? OffsetDetection::Status::NotInDatabase
                                                                        : OffsetDetection::Status::LookupFailed;
        d.error = lookup.error;
        return d;
    }
    return detectReadOffset(drive, toc, lookup.pressings, options);
}

// --- Offset source ------------------------------------------------------------------

std::string ReadOffsetSource::describe() const {
    switch (kind) {
        case Kind::Manual:
            return "manual";
        case Kind::Saved:
            return "saved for drive" + (detail.empty() ? std::string() : " " + detail);
        case Kind::Detected:
            return "auto-detected" + (detail.empty() ? std::string() : ": " + detail);
    }
    return {};
}

std::string readOffsetLogLine(int offset, const ReadOffsetSource& source) {
    return "Read offset correction: " + signedNumber(offset) + " samples (" + source.describe() + ")";
}

// --- Store ----------------------------------------------------------------------------

std::string driveOffsetKey(const DriveInfo& info) {
    return clean(info.vendor) + "|" + clean(info.product) + "|" + clean(info.revision);
}

DriveOffsetStore DriveOffsetStore::parse(const std::string& text) {
    DriveOffsetStore store;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos) continue;
        const size_t tab2 = line.find('\t', tab1 + 1);
        const std::string number = line.substr(0, tab1);
        const std::string key = line.substr(tab1 + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab1 - 1);
        if (key.empty() || number.empty() || number.size() > 7) continue;
        char* end = nullptr;
        const long v = std::strtol(number.c_str(), &end, 10);
        if (end == number.c_str() || *end != '\0') continue;
        if (std::labs(v) > long(100 * kSamplesPerSector)) continue;
        SavedDriveOffset saved;
        saved.offset = int(v);
        if (tab2 != std::string::npos) saved.note = line.substr(tab2 + 1);
        store.entries_[key] = saved;
    }
    return store;
}

std::string DriveOffsetStore::serialize() const {
    std::string out = "# cdreader read offsets per drive: offset<TAB>vendor|product|revision<TAB>note\n";
    for (const auto& [key, value] : entries_)
        out += std::to_string(value.offset) + "\t" + key + "\t" + value.note + "\n";
    return out;
}

const SavedDriveOffset* DriveOffsetStore::find(const std::string& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

void DriveOffsetStore::set(const std::string& key, const SavedDriveOffset& value) {
    std::string k = key;
    for (char& c : k)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    SavedDriveOffset v = value;
    v.note = cleanNote(v.note);
    entries_[k] = v;
}

bool DriveOffsetStore::erase(const std::string& key) { return entries_.erase(key) != 0; }

}  // namespace cdr
