#include "cdreader/drive_speed.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cdr {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint32_t plausibleOrZero(uint32_t kBps) { return plausibleCdSpeed(kBps) ? kBps : 0; }

}  // namespace

bool plausibleCdSpeed(uint32_t kBps) {
    return kBps >= uint32_t(kKBpsPerSpeed / 2) && kBps <= uint32_t(kMaxSpeedFactor * kKBpsPerSpeed + 0.5);
}

int speedFactor(uint32_t kBps) { return int(std::lround(double(kBps) / kKBpsPerSpeed)); }

uint16_t speedKBps(int factor) {
    if (factor <= 0) return kMaxSpeedKBps;
    const double kBps = std::ceil(factor * kKBpsPerSpeed - 1e-9);
    return uint16_t(std::min(kBps, double(kMaxSpeedKBps - 1)));
}

std::string formatSpeed(uint32_t kBps) { return std::to_string(speedFactor(kBps)) + "x"; }

// --- GET PERFORMANCE -----------------------------------------------------------

DiscPerformance DiscPerformance::parse(const uint8_t* data, size_t length) {
    DiscPerformance p;
    if (length < 8) {
        p.error = "GET PERFORMANCE response too short";
        return p;
    }
    // Performance data length excludes its own 4 bytes.
    const size_t end = size_t(std::min<uint64_t>(length, uint64_t(be32(data)) + 4));
    bool first = true;
    for (size_t at = 8; at + 16 <= end; at += 16) {
        const uint8_t* d = data + at;
        const uint32_t startLba = be32(d);
        const uint32_t start = be32(d + 4);
        const uint32_t endLba = be32(d + 8);
        const uint32_t stop = be32(d + 12);
        if (!plausibleCdSpeed(start) || !plausibleCdSpeed(stop) || endLba < startLba) continue;
        if (first) {
            p.startKBps = start;
            p.minKBps = std::min(start, stop);
            p.maxKBps = std::max(start, stop);
            first = false;
        }
        p.endKBps = stop;
        p.minKBps = std::min({p.minKBps, start, stop});
        p.maxKBps = std::max({p.maxKBps, start, stop});
    }
    p.valid = !first;
    if (!p.valid) p.error = end <= 8 ? "no performance descriptors" : "implausible performance descriptors";
    return p;
}

std::string DiscPerformance::range() const {
    if (!valid) return {};
    const int lo = speedFactor(minKBps);
    const int hi = speedFactor(maxKBps);
    return lo == hi ? std::to_string(lo) + "x" : std::to_string(lo) + "x-" + std::to_string(hi) + "x";
}

DiscPerformance readDiscPerformance(CdDrive& drive) {
    std::vector<uint8_t> data(8 + size_t(kPerformanceDescriptors) * 16);
    const ScsiResult r = drive.getPerformance(data.data(), kPerformanceDescriptors);
    if (!r.ok()) {
        DiscPerformance p;
        p.error = "GET PERFORMANCE failed: " + r.describe();
        return p;
    }
    return DiscPerformance::parse(data.data(), std::min(r.transferred, data.size()));
}

// --- What the drive reports ------------------------------------------------------

std::string DriveSpeedInfo::summary() const {
    std::string s;
    if (maxReadKBps) s = "max " + formatSpeed(maxReadKBps) + " (" + std::to_string(maxReadKBps) + " kB/s)";
    if (currentReadKBps) s += (s.empty() ? "" : ", ") + std::string("current ") + formatSpeed(currentReadKBps);
    if (performance.valid) s += (s.empty() ? "" : "; ") + std::string("disc performance ") + performance.range();
    return s.empty() ? "not reported by the drive" : s;
}

std::string DriveSpeedInfo::logLine() const { return "Read speed: " + summary(); }

DriveSpeedInfo driveSpeedInfo(const DriveCapabilities& caps, const DiscPerformance& performance) {
    DriveSpeedInfo info;
    if (caps.valid) {
        info.maxReadKBps = plausibleOrZero(caps.maxReadKBps);
        info.currentReadKBps = plausibleOrZero(caps.currentReadKBps);
    }
    info.performance = performance;
    return info;
}

DriveSpeedInfo readDriveSpeed(CdDrive& drive, const DriveCapabilities* caps) {
    const DriveCapabilities read = caps ? DriveCapabilities{} : drive.readCapabilities();
    return driveSpeedInfo(caps ? *caps : read, readDiscPerformance(drive));
}

// --- SET CD SPEED -------------------------------------------------------------------

std::string SpeedSetting::name() const {
    if (!requested) return "drive default";
    return factor ? std::to_string(factor) + "x" : "max";
}

bool parseSpeedSetting(const std::string& text, SpeedSetting& setting) {
    if (text == "max") {
        setting = {true, 0};
        return true;
    }
    std::string digits = text;
    if (!digits.empty() && (digits.back() == 'x' || digits.back() == 'X')) digits.pop_back();
    if (digits.empty() || digits.size() > 3) return false;
    int n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (n < 1 || n > kMaxSpeedFactor) return false;
    setting = {true, n};
    return true;
}

uint16_t SpeedChange::effectiveKBps() const {
    return result == Result::Applied || result == Result::Unknown ? setting.kBps() : kMaxSpeedKBps;
}

std::string SpeedChange::logLine() const {
    const std::string name = setting.name();
    switch (result) {
        case Result::NotRequested: return {};
        case Result::Applied: return "Read speed set to " + name + " (drive reports " + formatSpeed(afterKBps) + ")";
        case Result::Unknown:
            return "Read speed set to " + name + " (the drive does not report its current speed)";
        case Result::Ignored:
            return "Read speed: requested " + name + ", but the drive ignored the request (drive reports " +
                   formatSpeed(afterKBps) + ")";
        case Result::Rejected:
            return "Read speed: the drive rejected SET CD SPEED to " + name + " (" + detail + ")";
    }
    return {};
}

SpeedChange setReadSpeed(CdDrive& drive, const SpeedSetting& setting, const DriveCapabilities& before,
                         DriveCapabilities* after) {
    SpeedChange change;
    change.setting = setting;
    change.beforeKBps = before.valid ? plausibleOrZero(before.currentReadKBps) : 0;
    if (after) *after = before;
    if (!setting.requested) return change;

    const ScsiResult r = drive.setCdSpeed(setting.kBps());
    if (!r.ok()) {
        change.result = SpeedChange::Result::Rejected;
        change.detail = r.describe();
        return change;
    }
    const DriveCapabilities now = drive.readCapabilities();
    if (after) *after = now;
    change.afterKBps = now.valid ? plausibleOrZero(now.currentReadKBps) : 0;
    if (change.afterKBps == 0) {
        change.result = SpeedChange::Result::Unknown;
        return change;
    }
    // A drive picks the closest speed it supports; one that ignores the
    // command keeps reporting the speed it had.
    const int reported = speedFactor(change.afterKBps);
    const bool unchanged = change.beforeKBps != 0 && reported == speedFactor(change.beforeKBps);
    const bool reached = setting.factor == 0 ? reported >= speedFactor(change.beforeKBps) : reported == setting.factor;
    change.result = unchanged && !reached ? SpeedChange::Result::Ignored : SpeedChange::Result::Applied;
    return change;
}

// --- Measured speed -------------------------------------------------------------------

std::string formatMeasuredSpeed(uint64_t sectors, uint64_t micros) {
    if (micros == 0) return {};
    // kB/s = bytes * 1000 / micros (1 kB = 1000 bytes).
    const double kBps = double(sectors) * double(kSectorBytes) * 1000.0 / double(micros);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1fx (%.0f kB/s)", kBps / kKBpsPerSpeed, kBps);
    return buf;
}

std::string averageSpeedLogLine(uint64_t sectors, uint64_t micros, size_t tracks) {
    if (micros == 0) return {};
    const uint64_t seconds = (micros + 500000) / 1000000;
    char time[32];
    std::snprintf(time, sizeof time, "%u:%02u", unsigned(seconds / 60), unsigned(seconds % 60));
    return "Average read speed: " + formatMeasuredSpeed(sectors, micros) + ", " + std::to_string(tracks) +
           " track(s) in " + time;
}

}  // namespace cdr
