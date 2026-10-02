#pragma once

#include <map>
#include <string>

namespace cdr {

// A small persistent key=value store for user settings (#38), e.g. the CLI's
// %APPDATA%\cdreader\settings.txt. Text format, UTF-8, one entry per line:
//   key=value
// Blank lines and lines starting with '#' are ignored, as are lines without
// '=' or with an invalid key, so that a damaged file loses one entry at most.
// Spaces around the key and the value are removed. Keys are made of
// [a-z0-9._-]; values cannot hold line breaks (set() turns them into spaces).
// Unknown keys are kept, so that older and newer versions can share a file.
class SettingsStore {
public:
    static SettingsStore parse(const std::string& text);
    std::string serialize() const;

    static bool validKey(const std::string& key);

    const std::string* find(const std::string& key) const;
    std::string get(const std::string& key, const std::string& fallback = {}) const;
    // Throws std::invalid_argument for an invalid key. An empty value erases the key.
    void set(const std::string& key, const std::string& value);
    bool erase(const std::string& key);
    const std::map<std::string, std::string>& entries() const { return entries_; }

private:
    std::map<std::string, std::string> entries_;
};

}  // namespace cdr
