#include "cdreader/settings_store.h"

#include <stdexcept>

namespace cdr {

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

}  // namespace

bool SettingsStore::validKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

SettingsStore SettingsStore::parse(const std::string& text) {
    SettingsStore store;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        if (!validKey(key) || value.empty()) continue;
        store.entries_[key] = value;
    }
    return store;
}

std::string SettingsStore::serialize() const {
    std::string out = "# cdreader settings (key=value, see 'cdreader config')\n";
    for (const auto& [key, value] : entries_) out += key + "=" + value + "\n";
    return out;
}

const std::string* SettingsStore::find(const std::string& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

std::string SettingsStore::get(const std::string& key, const std::string& fallback) const {
    const std::string* v = find(key);
    return v ? *v : fallback;
}

void SettingsStore::set(const std::string& key, const std::string& value) {
    if (!validKey(key)) throw std::invalid_argument("invalid settings key '" + key + "'");
    std::string v = value;
    for (char& c : v)
        if (c == '\n' || c == '\r') c = ' ';
    v = trim(v);
    if (v.empty()) entries_.erase(key);
    else entries_[key] = v;
}

bool SettingsStore::erase(const std::string& key) { return entries_.erase(key) != 0; }

}  // namespace cdr
