#include "cdreader/rip_summary.h"

#include <algorithm>
#include <cstdio>

#include "cdreader/drive_speed.h"

namespace cdr {

namespace {

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

std::string numberList(const std::vector<int>& numbers) {
    std::string s;
    for (int n : numbers) s += (s.empty() ? "" : ", ") + std::to_string(n);
    return s;
}

// "v2 3" / "v1 2, v2 3": the submissions that match.
std::string matchCounts(const AccurateRipTrackResult& r) {
    std::string s;
    if (r.v1Confidence) s = "v1 " + std::to_string(r.v1Confidence);
    if (r.v2Confidence) s += (s.empty() ? "" : ", ") + std::string("v2 ") + std::to_string(r.v2Confidence);
    return s;
}

// The pressing lines are indented by 10 spaces; the re-read lines too.
constexpr const char* kIndent = "          ";

}  // namespace

// --- Re-read -------------------------------------------------------------------

TrackReread TrackReread::notSupported(int track) {
    TrackReread r;
    r.track = track;
    r.outcome = Outcome::NotSupported;
    return r;
}

std::vector<std::string> TrackReread::logLines() const {
    const std::string at = speedFactor > 0 ? "Re-read at " + std::to_string(speedFactor) + "x"
                                           : std::string("Re-read at the current speed");
    std::string line;
    switch (outcome) {
        case Outcome::Accurate:
            line = at + ": accurately ripped (" + matchCounts(accurateRip) + "), CRC32 " + hex32(result.crc32) +
                   " (first read " + hex32(firstCrc32) + ")" + (replaced ? ", file replaced" : "");
            break;
        case Outcome::SameData:
            line = at + ": same data (CRC32 " + hex32(result.crc32) +
                   ") - likely a different pressing or damage on the disc";
            break;
        case Outcome::DifferentData:
            line = at + ": different data (CRC32 " + hex32(result.crc32) + ", first read " + hex32(firstCrc32) +
                   "), not accurate either; the first read was kept";
            break;
        case Outcome::Failed:
            line = at + " failed: " + error + "; the first read was kept";
            break;
        case Outcome::NotSupported:
            return {kIndent + std::string("Re-read: not supported in single-file mode (rip the track on its own to "
                                          "re-read it)")};
    }
    std::vector<std::string> lines = {kIndent + line};
    if (outcome != Outcome::Failed && result.readProblems())
        lines.push_back(kIndent + std::string("Re-read had read problems: ") + result.readProblemText() +
                        " (retries " + std::to_string(result.retries) + ", " + result.status() + ")");
    return lines;
}

bool wantsAccurateRipReread(const AccurateRipTrackResult& ar, const TrackRipResult& result, int minConfidence) {
    if (!ar.inDatabase() || ar.accurate() || !result.readProblems()) return false;
    return std::any_of(ar.pressings.begin(), ar.pressings.end(),
                       [&](const AccurateRipPressingMatch& p) { return p.confidence >= minConfidence; });
}

int accurateRipRereadSpeed(const RipOptions& options) {
    if (!options.slowdown) return 0;
    int slowest = 0;
    for (int f : options.slowdownSpeeds)
        if (f > 0 && f <= kMaxSpeedFactor && (slowest == 0 || f < slowest)) slowest = f;
    return slowest;
}

std::string accurateRipRereadLogLine(bool enabled, const RipOptions& options) {
    const std::string head = "AccurateRip re-read: ";
    if (!enabled) return head + "disabled";
    const int factor = accurateRipRereadSpeed(options);
    if (factor == 0) return head + "not possible without slowdown";
    return head + "at " + std::to_string(factor) + "x (tracks with read problems that do not match)";
}

TrackReread rereadForAccurateRip(CdDrive& drive, const Toc& toc, const RipOptions& options, const Track& track,
                                 const std::vector<AccurateRipPressing>& pressings, uint32_t firstCrc32,
                                 const Ripper::SampleSink& sink, const Ripper::Progress& progress, Clock* clock) {
    TrackReread rr;
    rr.track = track.number;
    rr.firstCrc32 = firstCrc32;
    const int factor = accurateRipRereadSpeed(options);
    if (factor == 0) {
        rr.error = "slowdown is off";
        return rr;
    }
    // The speed now, as slowdownSteps() sees it: no command when the drive
    // already reads at the re-read speed or slower.
    uint32_t now = options.speedKBps == kMaxSpeedKBps ? 0 : options.speedKBps;
    if (options.currentSpeedKBps && (now == 0 || options.currentSpeedKBps < now)) now = options.currentSpeedKBps;
    const bool set = !(now && speedFactor(now) <= factor);
    RipOptions o = options;
    if (set) {
        rr.speedFactor = factor;
        const ScsiResult r = drive.setCdSpeed(speedKBps(factor));
        if (!r.ok()) {
            rr.error = "SET CD SPEED to " + std::to_string(factor) + "x failed (" + r.describe() + ")";
            return rr;
        }
        o.speedKBps = speedKBps(factor);
        o.currentSpeedKBps = speedKBps(factor);
    }
    // Back to the speed of the rip, also when the sink throws.
    struct Restore {
        CdDrive& drive;
        bool set;
        uint16_t kBps;
        ~Restore() {
            if (set) drive.setCdSpeed(kBps);
        }
    } restore{drive, set, options.speedKBps};

    Ripper ripper(drive, toc, o);
    if (clock) ripper.setClock(*clock);
    ripper.defeatCacheOnNextTrack();
    AccurateRipChecksum ar = AccurateRipChecksum::forTrack(toc, track);
    rr.result = ripper.ripTrack(
        track,
        [&](const uint8_t* pcm, size_t bytes) {
            ar.update(pcm, bytes);
            sink(pcm, bytes);
        },
        progress);
    rr.v1 = ar.v1();
    rr.v2 = ar.v2();
    rr.accurateRip = matchAccurateRip(pressings, accurateRipEntryIndex(toc, track), track.number, rr.v1, rr.v2);
    if (rr.accurateRip.accurate()) rr.outcome = TrackReread::Outcome::Accurate;
    else if (rr.result.crc32 == firstCrc32) rr.outcome = TrackReread::Outcome::SameData;
    else rr.outcome = TrackReread::Outcome::DifferentData;
    return rr;
}

// --- AccurateRipReport -----------------------------------------------------------

void AccurateRipReport::setLookup(const AccurateRipLookup& lookup, bool lookedUp) {
    error.clear();
    pressings = 0;
    if (!lookedUp) {
        status = Status::Disabled;
        return;
    }
    switch (lookup.status) {
        case AccurateRipLookup::Status::Found: status = Status::Found; break;
        case AccurateRipLookup::Status::NotFound: status = Status::NotFound; break;
        case AccurateRipLookup::Status::Error:
            status = Status::Error;
            error = lookup.error;
            break;
    }
    pressings = lookup.pressings.size();
}

const TrackReread* AccurateRipReport::reread(int track) const {
    auto it = rereads.find(track);
    return it == rereads.end() ? nullptr : &it->second;
}

bool AccurateRipReport::accurate(const AccurateRipTrackResult& t) const {
    if (t.accurate()) return true;
    const TrackReread* r = reread(t.track);
    return r != nullptr && r->accurate();
}

int AccurateRipReport::accurateTracks() const {
    int n = 0;
    for (const AccurateRipTrackResult& t : tracks) n += accurate(t) ? 1 : 0;
    return n;
}

int AccurateRipReport::tracksInDatabase() const {
    int n = 0;
    for (const AccurateRipTrackResult& t : tracks) n += t.inDatabase() ? 1 : 0;
    return n;
}

std::vector<std::string> AccurateRipReport::logLines(const std::string& disabledLine, const std::string& hint) const {
    std::vector<std::string> lines;
    const bool found = status == Status::Found;
    lines.push_back("AccurateRip (disc id " + id.toString() + ")" +
                    (found ? ", " + std::to_string(pressings) + " pressing(s) in database" : std::string()));
    std::map<std::string, int> byVersion;  // accurate tracks per matched checksum version
    int afterReread = 0;
    for (const AccurateRipTrackResult& r : tracks) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Track %02d  v1 %s  v2 %s  ", r.track, hex32(r.v1).c_str(), hex32(r.v2).c_str());
        lines.push_back(buf + (found ? r.describe() : std::string()));
        for (const AccurateRipPressingMatch& p : r.pressings) {
            std::snprintf(buf, sizeof buf, "%spressing %d: %s  confidence %3d  %s", kIndent, p.pressing,
                          hex32(p.checksum).c_str(), p.confidence,
                          p.version == 2 ? "v2 match" : p.version == 1 ? "v1 match" : "no match");
            lines.push_back(buf);
        }
        const TrackReread* rr = reread(r.track);
        if (rr != nullptr)
            for (std::string& l : rr->logLines()) lines.push_back(std::move(l));
        if (r.accurate()) {
            ++byVersion[r.matchedVersion()];
        } else if (rr != nullptr && rr->accurate()) {
            ++byVersion[rr->accurateRip.matchedVersion()];
            ++afterReread;
        }
    }

    switch (status) {
        case Status::Disabled:
            lines.push_back(disabledLine);
            break;
        case Status::NotFound:
            lines.push_back("AccurateRip: this disc is not in the database (no rips submitted yet)");
            break;
        case Status::Error:
            lines.push_back("AccurateRip: lookup failed: " + error);
            break;
        case Status::Found: {
            std::string versions;
            for (const auto& [version, count] : byVersion)
                versions += (versions.empty() ? "" : ", ") + version + ": " + std::to_string(count);
            if (afterReread) versions += "; " + std::to_string(afterReread) + " after a re-read";
            const int accurate = accurateTracks();
            const int inDatabase = tracksInDatabase();
            lines.push_back("AccurateRip: " + std::to_string(accurate) + " of " + std::to_string(tracks.size()) +
                            " track(s) accurately ripped" + (versions.empty() ? "" : " (" + versions + ")") + ", " +
                            std::to_string(inDatabase) + " track(s) in database");
            if (accurate == 0 && inDatabase > 0) lines.push_back(hint);
            break;
        }
    }
    return lines;
}

// --- Summary ---------------------------------------------------------------------

RipSummary summarizeRip(const std::vector<RippedTrackResult>& tracks, const AccurateRipReport* report) {
    RipSummary s;
    const bool found = report != nullptr && report->status == AccurateRipReport::Status::Found;
    for (const RippedTrackResult& t : tracks) {
        if (t.result == nullptr) continue;
        const TrackReread* rr = found ? report->reread(t.track) : nullptr;
        // A re-read that AccurateRip confirms replaced the file: its errors are gone.
        const bool replaced = rr != nullptr && rr->accurate() && rr->replaced;
        if (!t.result->clean() && !replaced) s.errorTracks.push_back(t.track);
        if (t.result->readProblems()) s.problemTracks[t.track] = t.result->readProblemText();
    }
    std::vector<int> confirmed;  // in the database and accurate
    if (found) {
        for (const AccurateRipTrackResult& ar : report->tracks) {
            const TrackReread* rr = report->reread(ar.track);
            if (rr != nullptr && rr->outcome != TrackReread::Outcome::NotSupported) {
                s.rereadTracks.push_back(ar.track);
                if (rr->accurate()) s.rereadAccurate.push_back(ar.track);
            }
            if (!ar.inDatabase()) continue;
            if (report->accurate(ar)) {
                confirmed.push_back(ar.track);
                continue;
            }
            s.notAccurate.push_back(ar.track);
            if (s.problemTracks.count(ar.track)) s.notAccurateWithProblems.push_back(ar.track);
        }
    }
    const bool allConfirmed = std::all_of(s.problemTracks.begin(), s.problemTracks.end(), [&](const auto& p) {
        return std::find(confirmed.begin(), confirmed.end(), p.first) != confirmed.end();
    });
    if (!s.errorTracks.empty() || !s.notAccurateWithProblems.empty()) s.verdict = RipSummary::Verdict::Errors;
    else if (s.problemTracks.empty() && s.notAccurate.empty()) s.verdict = RipSummary::Verdict::Clean;
    else if (s.notAccurate.empty() && allConfirmed) s.verdict = RipSummary::Verdict::Resolved;
    else s.verdict = RipSummary::Verdict::Warnings;
    return s;
}

std::string RipSummary::finalLine() const {
    switch (verdict) {
        case Verdict::Clean: return "All tracks ripped without errors";
        case Verdict::Resolved: return "All tracks ripped; the read problems were resolved (AccurateRip confirms the tracks)";
        case Verdict::Warnings: return "Finished with warnings";
        case Verdict::Errors: break;
    }
    return "Finished with errors";
}

std::vector<std::string> RipSummary::lines() const {
    std::vector<std::string> out;
    if (!errorTracks.empty())
        out.push_back("Tracks with unreadable sectors or suspicious positions: " + numberList(errorTracks));
    if (!problemTracks.empty()) {
        // "2, 4 (retries)" when the tracks share the reasons, else "2 (retries), 4 (C2 errors)".
        const std::string& first = problemTracks.begin()->second;
        const bool same = std::all_of(problemTracks.begin(), problemTracks.end(),
                                      [&](const auto& p) { return p.second == first; });
        std::string text;
        if (same) {
            std::vector<int> numbers;
            for (const auto& p : problemTracks) numbers.push_back(p.first);
            text = numberList(numbers) + " (" + first + ")";
        } else {
            for (const auto& [track, what] : problemTracks)
                text += (text.empty() ? "" : ", ") + std::to_string(track) + " (" + what + ")";
        }
        out.push_back("Tracks with read problems: " + text);
    }
    if (!rereadTracks.empty()) {
        std::string line = "Re-read after AccurateRip: " + std::to_string(rereadTracks.size()) + " track(s), ";
        if (rereadAccurate.empty()) line += "none accurately ripped";
        else
            line += std::to_string(rereadAccurate.size()) + " accurately ripped (track" +
                    (rereadAccurate.size() == 1 ? " " : "s ") + numberList(rereadAccurate) + ")";
        out.push_back(line);
    }
    if (!notAccurate.empty()) {
        std::string detail = (notAccurate.size() == 1 ? "track " : "tracks ") + numberList(notAccurate);
        if (!notAccurateWithProblems.empty()) {
            if (notAccurateWithProblems.size() == notAccurate.size()) {
                const bool retried = std::all_of(notAccurate.begin(), notAccurate.end(), [&](int t) {
                    return problemTracks.at(t).find("retries") != std::string::npos;
                });
                detail += retried ? "; read errors were retried" : "; all with read problems";
            } else {
                detail += "; read problems in " + numberList(notAccurateWithProblems);
            }
        }
        out.push_back(std::to_string(notAccurate.size()) + " track(s) not accurate in AccurateRip (" + detail + ")");
    }
    return out;
}

}  // namespace cdr
