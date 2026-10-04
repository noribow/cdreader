#include "cdreader/drive_offset_db.h"

#include <cctype>
#include <exception>

namespace cdr {

namespace {

int32_t le32(const unsigned char* p) {
    return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
}

std::string signedNumber(int v) { return (v > 0 ? "+" : "") + std::to_string(v); }

// Lower case, without whitespace.
std::string squeeze(const std::string& s) {
    std::string out;
    for (unsigned char c : s)
        if (!std::isspace(c) && c != 0) out.push_back(char(std::tolower(c)));
    return out;
}

// Looks like text markup rather than records (an error page).
bool looksLikeMarkup(const std::string& bytes) {
    size_t i = 0;
    while (i < bytes.size() && i < 64 && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (i < bytes.size() && bytes[i] == '<') return true;
    std::string head;
    for (size_t k = 0; k < bytes.size() && k < 512; ++k)
        head.push_back(char(std::tolower(static_cast<unsigned char>(bytes[k]))));
    return head.find("<html") != std::string::npos || head.find("<!doctype") != std::string::npos;
}

constexpr int kMaxSubmissions = 10000000;

}  // namespace

std::string DriveOffsetDbEntry::describe() const {
    std::string s = signedNumber(offset);
    std::string extra;
    if (submissions >= 0) extra = std::to_string(submissions) + (submissions == 1 ? " submission" : " submissions");
    if (agreePercent >= 0) extra += (extra.empty() ? "" : ", ") + std::to_string(agreePercent) + "% agree";
    return extra.empty() ? s : s + " (" + extra + ")";
}

DriveOffsetDb DriveOffsetDb::parse(const std::string& bytes) {
    DriveOffsetDb db;
    if (bytes.size() < kDriveOffsetRecordSize) {
        db.error = bytes.empty() ? "empty file" : "file too short (" + std::to_string(bytes.size()) + " bytes)";
        return db;
    }
    if (looksLikeMarkup(bytes)) {
        db.error = "not a drive offset file (text / HTML)";
        return db;
    }
    db.records = bytes.size() / kDriveOffsetRecordSize;
    size_t implausible = 0;  // records whose submission / percentage fields are out of range
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    for (size_t r = 0; r < db.records; ++r) {
        const unsigned char* rec = data + r * kDriveOffsetRecordSize;
        DriveOffsetDbEntry e;
        e.offset = int(int16_t(uint16_t(rec[0] | rec[1] << 8)));
        const unsigned char* name = rec + 2;
        size_t len = 0;
        while (len < kDriveOffsetNameSize && name[len] != 0) ++len;
        bool printable = true;
        for (size_t i = 0; i < len; ++i)
            if (name[i] < 0x20 || name[i] == 0x7f) printable = false;
        e.name.assign(reinterpret_cast<const char*>(name), len);
        while (!e.name.empty() && e.name.back() == ' ') e.name.pop_back();
        size_t lead = 0;
        while (lead < e.name.size() && e.name[lead] == ' ') ++lead;
        e.name.erase(0, lead);
        if (!printable || e.name.empty()) {
            ++db.badRecords;
            continue;
        }
        const unsigned char* misc = name + kDriveOffsetNameSize;
        const int32_t submissions = le32(misc);
        const int32_t percent = le32(misc + 4);
        const bool plausible = submissions >= 0 && submissions <= kMaxSubmissions && percent >= 0 && percent <= 100;
        if (plausible) {
            e.submissions = int(submissions);
            e.agreePercent = int(percent);
        } else {
            ++implausible;
        }
        db.entries.push_back(std::move(e));
    }
    if (db.entries.empty() || db.badRecords * 2 > db.records) {
        db.error = "not a drive offset file (" + std::to_string(db.badRecords) + " of " + std::to_string(db.records) +
                   " records unreadable)";
        db.entries.clear();
        return db;
    }
    // The submission / percentage layout is not documented: when it does not
    // fit the file, use none of it rather than some numbers that mean nothing.
    if (implausible * 10 > db.entries.size())
        for (DriveOffsetDbEntry& e : db.entries) e.submissions = e.agreePercent = -1;
    return db;
}

bool driveOffsetNameMatches(const std::string& dbName, const std::string& vendor, const std::string& product) {
    const std::string v = squeeze(vendor);
    const std::string p = squeeze(product);
    if (p.empty()) return false;
    const std::string n = squeeze(dbName);
    return n == v + "-" + p || n == v + p;
}

const DriveOffsetDbEntry* DriveOffsetDb::find(const std::string& vendor, const std::string& product) const {
    const DriveOffsetDbEntry* best = nullptr;
    for (const DriveOffsetDbEntry& e : entries)
        if (driveOffsetNameMatches(e.name, vendor, product) && (!best || e.submissions > best->submissions)) best = &e;
    return best;
}

bool driveOffsetDbCacheFresh(const DriveOffsetDbCache& cache, int64_t maxAgeSeconds) {
    return cache.present && cache.ageSeconds >= 0 && cache.ageSeconds <= maxAgeSeconds;
}

DriveOffsetDbLoad loadDriveOffsetDb(HttpClient* http, const DriveOffsetDbCache& cache, int64_t maxAgeSeconds) {
    DriveOffsetDbLoad load;
    DriveOffsetDb stored;
    if (cache.present) stored = DriveOffsetDb::parse(cache.bytes);
    if (driveOffsetDbCacheFresh(cache, maxAgeSeconds) && stored.valid()) {
        load.source = DriveOffsetDbLoad::Source::Cache;
        load.db = std::move(stored);
        return load;
    }
    if (http == nullptr) {
        load.error = "not downloaded";
    } else {
        try {
            HttpResponse r = http->get(kDriveOffsetDbUrl);
            if (!r.ok) {
                load.error = r.error.empty() ? "request failed" : r.error;
            } else if (r.status != 200) {
                load.error = "HTTP " + std::to_string(r.status);
            } else {
                DriveOffsetDb fetched = DriveOffsetDb::parse(r.body);
                if (fetched.valid()) {
                    load.source = DriveOffsetDbLoad::Source::Download;
                    load.db = std::move(fetched);
                    load.bytes = std::move(r.body);
                    return load;
                }
                load.error = "invalid DriveOffsets.bin: " + fetched.error;
            }
        } catch (const std::exception& e) {
            load.error = e.what();
        }
    }
    if (stored.valid()) {
        load.source = DriveOffsetDbLoad::Source::StaleCache;
        load.db = std::move(stored);
    }
    return load;
}

std::string DriveOffsetDbMatch::describe() const {
    switch (status) {
        case Status::NotChecked: return "not checked";
        case Status::Found: return entry.describe();
        case Status::NotListed: return "drive not listed";
        case Status::Unavailable: return "unavailable" + (error.empty() ? std::string() : " (" + error + ")");
    }
    return {};
}

std::string DriveOffsetDbMatch::logLine() const { return "AccurateRip drive database: " + describe(); }

std::string DriveOffsetDbMatch::logLine(int readOffsetUsed) const {
    std::string s = logLine();
    if (found() && entry.offset != readOffsetUsed) s += "; differs from the read offset used";
    return s;
}

std::string DriveOffsetDbMatch::agreementText() const {
    std::string s = "matches AccurateRip drive database";
    if (entry.submissions >= 0)
        s += " (" + std::to_string(entry.submissions) + (entry.submissions == 1 ? " submission)" : " submissions)");
    return s;
}

DriveOffsetDbMatch matchDriveOffsetDb(const DriveOffsetDbLoad& load, const DriveInfo& drive) {
    DriveOffsetDbMatch m;
    if (!load.available()) {
        m.status = DriveOffsetDbMatch::Status::Unavailable;
        m.error = load.error;
        return m;
    }
    if (const DriveOffsetDbEntry* e = load.db.find(drive.vendor, drive.product)) {
        m.status = DriveOffsetDbMatch::Status::Found;
        m.entry = *e;
    } else {
        m.status = DriveOffsetDbMatch::Status::NotListed;
    }
    return m;
}

}  // namespace cdr
