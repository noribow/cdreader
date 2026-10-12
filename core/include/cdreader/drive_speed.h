#pragma once

// Drive read speed (#49): what the drive supports and reads at (MODE SENSE
// page 2Ah, GET PERFORMANCE), setting it (SET CD SPEED, `--speed`), and the
// read speed measured during a rip. Shared by the CLI and the app.
//
// Speeds are in kB/s of 1000 bytes, as MMC reports them: 1x (44100 stereo
// 16-bit samples per second) is 176.4 kB/s; drives report 176 or 177 per x.

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"

namespace cdr {

constexpr double kKBpsPerSpeed = 176.4;  // 1x
// SET CD SPEED: as fast as the drive can.
constexpr uint16_t kMaxSpeedKBps = 0xFFFF;
// The highest speed --speed accepts (no CD drive reads faster than 56x).
constexpr int kMaxSpeedFactor = 72;

// A reported speed that makes sense for a CD: 0.5x .. 72x. Drives fill
// obsolete fields with 0, 0xFFFF or DVD rates; those count as unknown.
bool plausibleCdSpeed(uint32_t kBps);
// kB/s -> "x", rounded to the nearest integer (0 for 0): 8467 -> 48, 7040 -> 40.
int speedFactor(uint32_t kBps);
// "x" -> kB/s for SET CD SPEED, rounded up so that the drive does not pick
// the next lower speed: 8 -> 1412, 48 -> 8468 (at most 0xFFFE).
uint16_t speedKBps(int factor);
// "48x".
std::string formatSpeed(uint32_t kBps);

// GET PERFORMANCE type 00h (read, nominal performance): the speed range
// across the disc. A CAV drive reads faster towards the end of the disc
// ("16x-40x"), a CLV drive at one speed.
struct DiscPerformance {
    bool valid = false;  // at least one plausible descriptor
    uint32_t startKBps = 0;  // at the first descriptor's start LBA
    uint32_t endKBps = 0;    // at the last descriptor's end LBA
    uint32_t minKBps = 0;
    uint32_t maxKBps = 0;
    std::string error;  // why there is no range (valid false)

    // Parses the response: 8-byte header (performance data length in bytes
    // 0..3), then 16-byte descriptors (start LBA, start performance, end LBA,
    // end performance; 4 bytes each, big endian). Descriptors with
    // implausible speeds (0, DVD rates, garbage) are skipped.
    static DiscPerformance parse(const uint8_t* data, size_t length);
    // "16x-40x" (CAV), "24x" (CLV), empty when not valid.
    std::string range() const;
};

// Descriptors requested by readDiscPerformance().
constexpr uint16_t kPerformanceDescriptors = 8;

// GET PERFORMANCE; never throws (a drive without the command, or without a
// disc, gives valid false and the reason in error).
DiscPerformance readDiscPerformance(CdDrive& drive);

// What the drive tells about its read speed.
struct DriveSpeedInfo {
    uint32_t maxReadKBps = 0;      // page 2Ah bytes 8..9; 0: not reported / implausible
    uint32_t currentReadKBps = 0;  // page 2Ah bytes 14..15; 0: not reported / implausible
    DiscPerformance performance;   // GET PERFORMANCE

    bool known() const { return maxReadKBps || currentReadKBps || performance.valid; }
    // "max 48x (8467 kB/s), current 24x; disc performance 16x-40x" (the
    // parts that are known), "not reported by the drive" otherwise.
    std::string summary() const;
    // "Read speed: " + summary().
    std::string logLine() const;
};

// From capabilities read before and a GET PERFORMANCE result.
DriveSpeedInfo driveSpeedInfo(const DriveCapabilities& caps, const DiscPerformance& performance);
// MODE SENSE (unless `caps` is given) and GET PERFORMANCE. Never throws.
DriveSpeedInfo readDriveSpeed(CdDrive& drive, const DriveCapabilities* caps = nullptr);

// --speed max|<N>.
struct SpeedSetting {
    bool requested = false;  // false: the drive is left at its speed (no command)
    int factor = 0;          // 0: max, otherwise N x

    uint16_t kBps() const { return factor ? speedKBps(factor) : kMaxSpeedKBps; }
    // "max", "8x"; "drive default" when not requested.
    std::string name() const;
};
// "max", or N / Nx with N in 1..kMaxSpeedFactor. False for anything else.
bool parseSpeedSetting(const std::string& text, SpeedSetting& setting);

// The outcome of setReadSpeed().
struct SpeedChange {
    enum class Result {
        NotRequested,  // no command sent
        Applied,       // the drive reports the new speed (or the closest it supports)
        Unknown,       // accepted, but the drive does not report its current speed
        Ignored,       // accepted, but the drive still reports the speed it had
        Rejected,      // SET CD SPEED failed (see detail)
    };
    SpeedSetting setting;
    Result result = Result::NotRequested;
    uint32_t beforeKBps = 0;  // current read speed before (0: not reported)
    uint32_t afterKBps = 0;   // ... after the command
    std::string detail;       // Rejected: the drive's answer

    // The read speed rips run at as far as we know: the requested speed
    // unless the drive rejected or ignored it (kMaxSpeedKBps: the drive's own).
    uint16_t effectiveKBps() const;
    // "Read speed set to 8x (drive reports 8x)", "Read speed set to max
    // (drive reports 48x)", "Read speed set to 8x (the drive does not report
    // its current speed)", "Read speed: requested 8x, but the drive ignored
    // the request (drive reports 24x)", "Read speed: the drive rejected SET
    // CD SPEED to 8x (...)"; empty when not requested.
    std::string logLine() const;
};

// Sends SET CD SPEED for `setting` (nothing when not requested), then MODE
// SENSE again to see what the drive did; `before` is the page read before
// the command. `after` (optional) receives the new page. Never throws.
SpeedChange setReadSpeed(CdDrive& drive, const SpeedSetting& setting, const DriveCapabilities& before,
                         DriveCapabilities* after = nullptr);

// Measured read speed: `sectors` of 2352 bytes read in `micros`.
// "24.3x (4286 kB/s)", empty when micros is 0.
std::string formatMeasuredSpeed(uint64_t sectors, uint64_t micros);
// "Average read speed: 24.3x (4286 kB/s), 3 track(s) in 0:42", empty when
// micros is 0. For the end of rip.log.
std::string averageSpeedLogLine(uint64_t sectors, uint64_t micros, size_t tracks);

}  // namespace cdr
