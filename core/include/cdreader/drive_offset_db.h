#pragma once

// The AccurateRip drive offset database (#37): DriveOffsets.bin, the list of
// read offsets submitted per drive model that EAC, dBpoweramp and CUETools
// use. Read offset detection (offset_detect.h) uses it to decide between
// pressings shifted against each other: the offset is a property of the
// drive, so the candidate the database lists for this drive is the one.
//
// File format (no official specification; derived from CUETools'
// AccurateRipVerify.FindDriveReadOffset, which reads the same file):
// a sequence of fixed-size records of 0x45 = 69 bytes, no header:
//
//   +0   int16 LE   correction offset in samples (the EAC / AccurateRip
//                   convention, as cdreader's --offset)
//   +2   char[33]   drive name, ASCII, NUL / space padded: INQUIRY vendor
//                   and product joined by " - ", e.g. "HL-DT-ST - BD-RE  BP71N"
//                   (CUETools compares it with vendor.TrimEnd() + " - " +
//                   product.TrimEnd())
//   +35  34 bytes   other data. Read here as int32 LE submission count at +35
//                   and int32 LE "percentage agree" at +39 (the columns of
//                   accuraterip.com/driveoffsets.htm). This part is NOT
//                   verified against a published description: values outside
//                   0..10,000,000 / 0..100 are treated as unknown (-1), and
//                   when many records have such values the interpretation is
//                   dropped for the whole file. Nothing but the "prefer the
//                   entry with most submissions" tie-break and the log text
//                   depends on it.
//
// Offset and name are what other software relies on; the parser is
// defensive: a trailing partial record is ignored, records with an empty or
// non-printable name are skipped, and a body that is mostly junk (e.g. an
// HTML error page delivered with status 200) is rejected as a whole.
//
// Core only parses bytes and decides about the cache; the platforms store
// the file (Android: cacheDir, CLI: the settings folder).

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/cd_drive.h"
#include "cdreader/http.h"

namespace cdr {

inline constexpr const char* kDriveOffsetDbUrl = "http://www.accuraterip.com/accuraterip/DriveOffsets.bin";
inline constexpr size_t kDriveOffsetRecordSize = 0x45;
inline constexpr size_t kDriveOffsetNameSize = 0x21;
// A downloaded file is used this long before it is fetched again.
inline constexpr int64_t kDriveOffsetDbMaxAgeSeconds = 30LL * 24 * 60 * 60;

struct DriveOffsetDbEntry {
    int offset = 0;
    std::string name;       // as in the file, trailing NULs / spaces removed
    int submissions = -1;   // -1: unknown
    int agreePercent = -1;  // -1: unknown

    // "+6 (1234 submissions, 100% agree)", parts left out when unknown.
    std::string describe() const;
};

struct DriveOffsetDb {
    std::vector<DriveOffsetDbEntry> entries;
    size_t records = 0;     // complete records in the file
    size_t badRecords = 0;  // skipped (empty / non-printable name)
    std::string error;      // why the file is unusable (empty: usable)

    bool valid() const { return error.empty() && !entries.empty(); }

    static DriveOffsetDb parse(const std::string& bytes);

    // The entry for the drive (driveOffsetNameMatches()); with several, the
    // one with most submissions (the first of equals). Null if not listed.
    const DriveOffsetDbEntry* find(const std::string& vendor, const std::string& product) const;
};

// Whether a database name denotes the drive with these INQUIRY strings:
// case-insensitive and ignoring whitespace, either "vendor - product" (the
// AccurateRip form, any spacing around the dash) or "vendor product" (EAC's).
bool driveOffsetNameMatches(const std::string& dbName, const std::string& vendor, const std::string& product);

// The file the platform has stored, if any.
struct DriveOffsetDbCache {
    bool present = false;
    std::string bytes;
    int64_t ageSeconds = 0;  // now - last write; negative (clock moved back) counts as stale
};

// Fresh: present, 0 <= age <= maxAge.
bool driveOffsetDbCacheFresh(const DriveOffsetDbCache& cache, int64_t maxAgeSeconds = kDriveOffsetDbMaxAgeSeconds);

struct DriveOffsetDbLoad {
    enum class Source {
        None,        // unavailable (see error)
        Cache,       // the stored file, fresh
        Download,    // downloaded now: the platform should store `bytes`
        StaleCache,  // the download failed, the older stored file is used
    };
    Source source = Source::None;
    DriveOffsetDb db;
    std::string bytes;  // Download: the file to store
    std::string error;  // why the download failed (also with StaleCache)

    bool available() const { return source != Source::None; }
    bool shouldStore() const { return source == Source::Download; }
};

// The fresh stored file if valid; otherwise downloads kDriveOffsetDbUrl
// (`http` null: no download) and falls back to a stale stored file. Never throws.
DriveOffsetDbLoad loadDriveOffsetDb(HttpClient* http, const DriveOffsetDbCache& cache,
                                    int64_t maxAgeSeconds = kDriveOffsetDbMaxAgeSeconds);

// The drive's entry in the database, for detection and the logs.
struct DriveOffsetDbMatch {
    enum class Status {
        NotChecked,   // the database was not consulted
        Found,
        NotListed,    // the database has no entry for the drive
        Unavailable,  // download failed and nothing stored (see error)
    };
    Status status = Status::NotChecked;
    DriveOffsetDbEntry entry;  // Found
    std::string error;         // Unavailable; also a failed refresh of a stale file

    bool found() const { return status == Status::Found; }
    // "+6 (1234 submissions)" / "drive not listed" / "unavailable (no network)" / "not checked".
    std::string describe() const;
    // "AccurateRip drive database: +6 (1234 submissions, 100% agree)".
    std::string logLine() const;
    // rip.log: logLine(), with "; differs from the read offset used" when
    // the database lists another offset than `readOffsetUsed`.
    std::string logLine(int readOffsetUsed) const;
    // "matches AccurateRip drive database (1234 submissions)" (Found).
    std::string agreementText() const;
};

DriveOffsetDbMatch matchDriveOffsetDb(const DriveOffsetDbLoad& load, const DriveInfo& drive);

}  // namespace cdr
