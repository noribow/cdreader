#include "cdreader/cddb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <utility>

namespace cdr {

namespace {

std::vector<std::string> splitLines(const std::string& body) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// Three digit response code at the start of a status line, or 0.
int responseCode(const std::string& line) {
    if (line.size() < 3) return 0;
    for (int i = 0; i < 3; ++i)
        if (line[size_t(i)] < '0' || line[size_t(i)] > '9') return 0;
    if (line.size() > 3 && line[3] != ' ') return 0;
    return std::atoi(line.substr(0, 3).c_str());
}

// "categ discid dtitle"
bool parseMatchLine(const std::string& line, CddbMatch& m) {
    const size_t s1 = line.find(' ');
    if (s1 == std::string::npos || s1 == 0) return false;
    const size_t s2 = line.find(' ', s1 + 1);
    m.category = line.substr(0, s1);
    m.discId = line.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
    m.title = s2 == std::string::npos ? std::string() : toValidUtf8(trim(line.substr(s2 + 1)));
    return !m.discId.empty();
}

// Decodes \n, \t and \\ (other backslashes are kept as they are).
std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[i + 1];
            if (c == 'n' || c == 't' || c == '\\') {
                out += c == 'n' ? '\n' : c == 't' ? '\t' : '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Value of a single-line field: decoded, control characters (from \n, \t) as
// spaces, surrounding whitespace removed.
std::string fieldValue(const std::string& raw) {
    std::string v = unescape(toValidUtf8(raw));
    for (char& c : v)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
    return trim(v);
}

// Splits "Artist / Title" at the first " / ".
bool splitArtistTitle(const std::string& s, std::string& artist, std::string& title) {
    const size_t sep = s.find(" / ");
    if (sep == std::string::npos) return false;
    artist = trim(s.substr(0, sep));
    title = trim(s.substr(sep + 3));
    return true;
}

bool isVariousArtists(const std::string& artist) {
    std::string lower;
    for (char c : artist) lower += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return lower.rfind("various", 0) == 0;
}

// First non-blank line of a response body, without the line break.
std::string firstLine(const std::string& body) {
    for (const std::string& line : splitLines(body))
        if (!trim(line).empty()) return trim(line);
    return {};
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool printableAscii(const std::string& s) {
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7F) return false;
    }
    return true;
}

bool isAlnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

}  // namespace

// --- Server presets (#46) -------------------------------------------------------

const std::vector<CddbServerPreset>& cddbServerPresets() {
    static const std::vector<CddbServerPreset> presets = {
        {kCddbServerGnudb, "GNUDB", kDefaultCddbServer},
        {kCddbServerJapdb, "JAPDB", kJapdbServer},
    };
    return presets;
}

const CddbServerPreset* findCddbServerPreset(const std::string& name) {
    const std::string id = lower(trim(name));
    for (const CddbServerPreset& p : cddbServerPresets())
        if (id == p.id) return &p;
    return nullptr;
}

std::string normalizeCddbServerUrl(const std::string& url) {
    const std::string u = trim(url);
    const std::string l = lower(u);
    size_t schemeEnd = 0;
    const char* defaultPort = nullptr;
    if (l.rfind("http://", 0) == 0) {
        schemeEnd = 7;
        defaultPort = ":80";
    } else if (l.rfind("https://", 0) == 0) {
        schemeEnd = 8;
        defaultPort = ":443";
    } else {
        return u;
    }
    const size_t authorityEnd = std::min(u.find_first_of("/?", schemeEnd), u.size());
    std::string authority = l.substr(schemeEnd, authorityEnd - schemeEnd);
    const std::string port = defaultPort;
    if (authority.size() > port.size() && authority.compare(authority.size() - port.size(), port.size(), port) == 0)
        authority.erase(authority.size() - port.size());
    const size_t queryStart = std::min(u.find('?', authorityEnd), u.size());
    std::string path = u.substr(authorityEnd, queryStart - authorityEnd);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return l.substr(0, schemeEnd) + authority + path + u.substr(queryStart);
}

const CddbServerPreset* cddbServerPresetForUrl(const std::string& url) {
    const std::string n = normalizeCddbServerUrl(url);
    for (const CddbServerPreset& p : cddbServerPresets())
        if (n == normalizeCddbServerUrl(p.url)) return &p;
    return nullptr;
}

std::string resolveCddbServer(const std::string& value) {
    const std::string v = trim(value);
    if (v.empty()) return kDefaultCddbServer;
    if (const CddbServerPreset* p = findCddbServerPreset(v)) return p->url;
    return v;
}

std::string cddbServerSettingValue(const std::string& value) {
    const std::string v = trim(value);
    if (v.empty()) return {};
    if (const CddbServerPreset* p = findCddbServerPreset(v)) return p->id;
    if (const CddbServerPreset* p = cddbServerPresetForUrl(v)) return p->id;
    return v;
}

std::string cddbServerChoice(const std::string& value) {
    if (trim(value).empty()) return cddbServerPresets().front().id;
    const std::string stored = cddbServerSettingValue(value);
    return findCddbServerPreset(stored) ? stored : kCddbServerCustom;
}

std::string cddbServerDisplayName(const std::string& server) {
    const CddbServerPreset* p = findCddbServerPreset(server);
    if (p) return std::string(p->label) + ", " + p->url;
    p = cddbServerPresetForUrl(server);
    return p ? std::string(p->label) + ", " + trim(server) : server;
}

std::string toValidUtf8(const std::string& s) {
    bool valid = true;
    for (size_t i = 0; i < s.size() && valid;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            valid = false;
            break;
        }
        if (i + len > s.size()) {
            valid = false;
            break;
        }
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) valid = false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // Overlong forms, UTF-16 surrogates and code points past U+10FFFF.
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            valid = false;
        i += len;
    }
    if (valid) return s;

    std::string out;
    out.reserve(s.size() * 2);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += ch;
        } else {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string cddbQueryCommand(const Toc& toc) {
    std::string cmd = "cddb query " + hex32(toc.cddbId()) + " " + std::to_string(toc.tracks.size());
    for (const Track& t : toc.tracks) cmd += " " + std::to_string(t.startLba + kPregapSectors);
    cmd += " " + std::to_string((toc.leadOutLba + kPregapSectors) / kSectorsPerSecond);
    return cmd;
}

std::string cddbReadCommand(const CddbMatch& match) { return "cddb read " + match.category + " " + match.discId; }

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += ch;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string cddbRequestUrl(const std::string& server, const std::string& command, const CddbClientInfo& client) {
    // The hello fields are separated by spaces, so they must not contain any.
    auto field = [](std::string s) {
        for (char& c : s)
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) c = '_';
        return s.empty() ? std::string("-") : s;
    };
    const std::string hello = field(client.user) + " " + field(client.host) + " " + field(client.name) + " " +
                              field(client.version);
    const char joiner = server.find('?') == std::string::npos ? '?' : '&';
    return server + joiner + "cmd=" + urlEncode(command) + "&hello=" + urlEncode(hello) + "&proto=6";
}

CddbQueryResult parseCddbQueryResponse(const std::string& body) {
    CddbQueryResult result;
    const std::vector<std::string> lines = splitLines(body);
    size_t i = 0;
    while (i < lines.size() && trim(lines[i]).empty()) ++i;
    if (i == lines.size()) {
        result.message = "empty response";
        return result;
    }
    const std::string& status = lines[i];
    result.code = responseCode(status);
    result.message = toValidUtf8(trim(status));

    switch (result.code) {
    case 200: {
        CddbMatch m;
        if (!parseMatchLine(trim(status.substr(4)), m)) {
            result.message = "malformed match: " + result.message;
            return result;
        }
        result.matches.push_back(m);
        result.status = CddbQueryResult::Status::Exact;
        return result;
    }
    case 210:
    case 211: {
        for (++i; i < lines.size() && lines[i] != "."; ++i) {
            CddbMatch m;
            if (parseMatchLine(trim(lines[i]), m)) result.matches.push_back(m);
        }
        if (result.matches.empty()) {
            result.message = "match list is empty: " + result.message;
            return result;
        }
        result.status = result.code == 210 ? CddbQueryResult::Status::Exact : CddbQueryResult::Status::Inexact;
        return result;
    }
    case 202:
        result.status = CddbQueryResult::Status::NotFound;
        return result;
    default:
        if (result.code == 0) result.message = "not a CDDB response: " + result.message.substr(0, 80);
        return result;
    }
}

AlbumMetadata parseXmcd(const std::string& text) {
    std::string dtitle, dyear, dgenre;
    std::map<size_t, std::string> ttitles;  // raw values, continuation lines concatenated

    for (const std::string& line : splitLines(text)) {
        if (line.empty() || line[0] == '#' || line == ".") continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = line.substr(eq + 1);
        if (key == "DTITLE") dtitle += value;
        else if (key == "DYEAR") dyear += value;
        else if (key == "DGENRE") dgenre += value;
        else if (key.rfind("TTITLE", 0) == 0 && key.size() > 6 && key.size() <= 9 &&
                 key.find_first_not_of("0123456789", 6) == std::string::npos)
            ttitles[size_t(std::atoi(key.c_str() + 6))] += value;
    }

    AlbumMetadata album;
    const std::string disc = fieldValue(dtitle);
    if (!splitArtistTitle(disc, album.artist, album.title)) {
        // Per the xmcd spec, no separator means artist and title are the same.
        album.artist = disc;
        album.title = disc;
    }
    album.year = fieldValue(dyear);
    album.genre = fieldValue(dgenre);

    const size_t count = ttitles.empty() ? 0 : ttitles.rbegin()->first + 1;
    album.trackTitles.assign(count, std::string());
    for (const auto& [index, raw] : ttitles) album.trackTitles[index] = fieldValue(raw);

    // Compilations put "Artist / Title" in every track title. Only split when
    // the album says so or every named track follows that form, so that a
    // single title which happens to contain " / " is left alone.
    size_t named = 0, withArtist = 0;
    for (const std::string& t : album.trackTitles) {
        if (t.empty()) continue;
        ++named;
        if (t.find(" / ") != std::string::npos) ++withArtist;
    }
    if (withArtist > 0 && (isVariousArtists(album.artist) || withArtist == named)) {
        album.trackArtists.assign(count, std::string());
        for (size_t i = 0; i < count; ++i) {
            std::string artist, title;
            if (splitArtistTitle(album.trackTitles[i], artist, title)) {
                album.trackArtists[i] = artist;
                album.trackTitles[i] = title;
            }
        }
    }
    return album;
}

bool parseCddbReadResponse(const std::string& body, AlbumMetadata& album, std::string& error) {
    const std::vector<std::string> lines = splitLines(body);
    size_t i = 0;
    while (i < lines.size() && trim(lines[i]).empty()) ++i;
    if (i == lines.size()) {
        error = "empty response";
        return false;
    }
    const int code = responseCode(lines[i]);
    if (code != 210) {
        error = code ? toValidUtf8(trim(lines[i])) : "not a CDDB response: " + toValidUtf8(trim(lines[i])).substr(0, 80);
        return false;
    }
    std::string entry;
    for (++i; i < lines.size() && lines[i] != "."; ++i) entry += lines[i] + "\n";
    album = parseXmcd(entry);
    return true;
}

CddbLookupResult lookupCddb(HttpClient& http, const Toc& toc, const CddbOptions& options) {
    CddbLookupResult result;
    if (toc.tracks.empty()) {
        result.error = "empty TOC";
        return result;
    }
    auto fetch = [&](const std::string& command, std::string& body) {
        const HttpResponse r = http.get(cddbRequestUrl(options.server, command, options.client));
        if (!r.ok) {
            result.error = r.error.empty() ? "HTTP request failed" : r.error;
            return false;
        }
        if (r.status != 200) {
            result.error = "HTTP status " + std::to_string(r.status);
            // Some servers put a CDDB status line in an error page.
            const std::string line = firstLine(r.body);
            if (responseCode(line)) {
                result.error += ": " + toValidUtf8(line);
                result.hint = cddbErrorHint(line);
            }
            return false;
        }
        body = r.body;
        return true;
    };

    try {
        std::string body;
        if (!fetch(cddbQueryCommand(toc), body)) return result;
        const CddbQueryResult query = parseCddbQueryResponse(body);
        if (query.status == CddbQueryResult::Status::NotFound) {
            result.error = "no matching disc in the database";
            return result;
        }
        if (query.status == CddbQueryResult::Status::Error) {
            result.error = "query failed: " + query.message;
            result.hint = cddbErrorHint(query.message);
            return result;
        }
        result.matches = query.matches;
        result.exact = query.status == CddbQueryResult::Status::Exact;
        result.chosen = options.matchIndex < result.matches.size() ? options.matchIndex : 0;

        if (!fetch(cddbReadCommand(result.matches[result.chosen]), body)) return result;
        AlbumMetadata album;
        std::string error;
        if (!parseCddbReadResponse(body, album, error)) {
            result.error = "read failed: " + error;
            result.hint = cddbErrorHint(error);
            return result;
        }

        // TTITLEn is the n-th track of the TOC; AlbumMetadata is indexed by
        // track number.
        const size_t skip = size_t(toc.tracks.front().number > 1 ? toc.tracks.front().number - 1 : 0);
        album.trackTitles.insert(album.trackTitles.begin(), skip, std::string());
        if (!album.trackArtists.empty()) album.trackArtists.insert(album.trackArtists.begin(), skip, std::string());

        char id[16];
        std::snprintf(id, sizeof id, "%08X", toc.cddbId());
        album.discId = id;
        result.album = album;
        result.found = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

std::vector<std::string> cddbLookupLogLines(bool enabled, const std::string& server, const CddbLookupResult& result) {
    std::vector<std::string> lines;
    if (!enabled) {
        lines.push_back("CDDB lookup: disabled");
        return lines;
    }
    const std::string name = cddbServerDisplayName(server);
    if (!result.found) {
        lines.push_back("CDDB lookup (" + name + "): " + result.error);
        if (result.hint != CddbHint::None) lines.push_back(cddbHintText(result.hint));
        return lines;
    }
    lines.push_back("CDDB lookup (" + name + "): " + std::to_string(result.matches.size()) +
                    (result.exact ? " exact" : " inexact") + " match(es)");
    for (size_t i = 0; i < result.matches.size(); ++i) {
        const CddbMatch& m = result.matches[i];
        lines.push_back((i == result.chosen ? "  * " : "    ") + std::to_string(i + 1) + ". " + m.category + "/" +
                        m.discId + "  " + m.title);
    }
    lines.push_back("Artist: " + result.album.artist);
    lines.push_back("Album: " + result.album.title);
    lines.push_back("Year: " + result.album.year);
    lines.push_back("Genre: " + result.album.genre);
    return lines;
}

// --- Settings (#38) ------------------------------------------------------------

CddbHint cddbErrorHint(const std::string& responseLine) {
    const std::string line = trim(responseLine);
    const int code = responseCode(line);
    if (code == 0) return CddbHint::None;
    if (code == 409 || code == 431) return CddbHint::ContactEmail;
    const std::string text = lower(line);
    for (const char* word : {"email", "e-mail", "mail address", "hello", "handshake", "unknown application"})
        if (text.find(word) != std::string::npos) return CddbHint::ContactEmail;
    return CddbHint::None;
}

std::string cddbHintText(CddbHint hint) {
    switch (hint) {
        case CddbHint::None: break;
        case CddbHint::ContactEmail:
            return "Hint: the CDDB server refused the greeting (hello). gnudb.org requires a contact e-mail "
                   "address: set a contact e-mail address in the CDDB settings (CLI: 'cdreader config cddb-email "
                   "<user@host>' or --cddb-hello; Android: Advanced settings > CDDB).";
    }
    return {};
}

CddbConfig cddbConfigFromSettings(const SettingsStore& store) {
    CddbConfig c;
    c.server = store.get(kSettingCddbServer);
    c.email = store.get(kSettingCddbEmail);
    c.appName = store.get(kSettingCddbAppName);
    c.appVersion = store.get(kSettingCddbAppVersion);
    return c;
}

void storeCddbConfig(SettingsStore& store, const CddbConfig& config) {
    store.set(kSettingCddbServer, config.server);
    store.set(kSettingCddbEmail, config.email);
    store.set(kSettingCddbAppName, config.appName);
    store.set(kSettingCddbAppVersion, config.appVersion);
}

CddbConfigProblem checkCddbServer(const std::string& url) {
    if (url.empty() || findCddbServerPreset(url)) return CddbConfigProblem::None;
    const std::string l = lower(url);
    size_t schemeEnd = 0;
    if (l.rfind("http://", 0) == 0) schemeEnd = 7;
    else if (l.rfind("https://", 0) == 0) schemeEnd = 8;
    else return CddbConfigProblem::ServerScheme;
    if (!printableAscii(url) || url.find('#') != std::string::npos) return CddbConfigProblem::ServerCharacters;
    const size_t authorityEnd = url.find_first_of("/?", schemeEnd);
    const std::string authority =
        url.substr(schemeEnd, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - schemeEnd);
    if (authority.empty() || authority.find('@') != std::string::npos) return CddbConfigProblem::ServerHost;
    std::string host = authority;
    const size_t bracket = authority.rfind(']');
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
            std::atoi(port.c_str()) < 1 || std::atoi(port.c_str()) > 65535)
            return CddbConfigProblem::ServerHost;
        host = authority.substr(0, colon);
    }
    if (host.empty()) return CddbConfigProblem::ServerHost;
    if (host.front() == '[') return host.back() == ']' && host.size() > 2 ? CddbConfigProblem::None
                                                                            : CddbConfigProblem::ServerHost;
    for (char c : host)
        if (!isAlnum(c) && c != '.' && c != '-') return CddbConfigProblem::ServerHost;
    if (host.front() == '.' || host.front() == '-' || host.find("..") != std::string::npos)
        return CddbConfigProblem::ServerHost;
    return CddbConfigProblem::None;
}

CddbConfigProblem checkCddbEmail(const std::string& email) {
    if (email.empty()) return CddbConfigProblem::None;
    if (email.size() > 254 || !printableAscii(email)) return CddbConfigProblem::EmailShape;
    const size_t at = email.find('@');
    if (at == std::string::npos || email.find('@', at + 1) != std::string::npos) return CddbConfigProblem::EmailShape;
    const std::string user = email.substr(0, at);
    const std::string host = email.substr(at + 1);
    // Local part: the unquoted characters of RFC 5322, no leading / trailing / double dots.
    if (user.empty() || user.size() > 64 || user.front() == '.' || user.back() == '.' ||
        user.find("..") != std::string::npos)
        return CddbConfigProblem::EmailShape;
    for (char c : user)
        if (!isAlnum(c) && std::string(".!#$%&'*+/=?^_`{|}~-").find(c) == std::string::npos)
            return CddbConfigProblem::EmailShape;
    // Domain: at least two labels of letters, digits and hyphens.
    if (host.empty() || host.size() > 253 || host.find('.') == std::string::npos) return CddbConfigProblem::EmailShape;
    size_t pos = 0;
    while (pos <= host.size()) {
        size_t dot = host.find('.', pos);
        if (dot == std::string::npos) dot = host.size();
        const std::string label = host.substr(pos, dot - pos);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return CddbConfigProblem::EmailShape;
        for (char c : label)
            if (!isAlnum(c) && c != '-') return CddbConfigProblem::EmailShape;
        pos = dot + 1;
    }
    return CddbConfigProblem::None;
}

CddbConfigProblem checkCddbHelloField(const std::string& value) {
    if (value.empty()) return CddbConfigProblem::None;
    return value.size() <= 64 && printableAscii(value) ? CddbConfigProblem::None : CddbConfigProblem::HelloCharacters;
}

std::string describeCddbConfigProblem(CddbConfigProblem problem) {
    switch (problem) {
        case CddbConfigProblem::None: break;
        case CddbConfigProblem::ServerScheme: return "the server must be an http:// or https:// URL";
        case CddbConfigProblem::ServerHost: return "the server URL has no valid host name";
        case CddbConfigProblem::ServerCharacters:
            return "the server URL must not contain spaces, control characters, non-ASCII characters or '#'";
        case CddbConfigProblem::EmailShape: return "expected an e-mail address such as user@example.com";
        case CddbConfigProblem::HelloCharacters:
            return "only printable ASCII characters without spaces are allowed (at most 64)";
    }
    return {};
}

std::string cddbConfigError(const CddbConfig& config) {
    const std::pair<const char*, CddbConfigProblem> checks[] = {
        {kSettingCddbServer, checkCddbServer(config.server)},
        {kSettingCddbEmail, checkCddbEmail(config.email)},
        {kSettingCddbAppName, checkCddbHelloField(config.appName)},
        {kSettingCddbAppVersion, checkCddbHelloField(config.appVersion)},
    };
    for (const auto& [key, problem] : checks)
        if (problem != CddbConfigProblem::None) return std::string(key) + ": " + describeCddbConfigProblem(problem);
    return {};
}

bool splitCddbEmail(const std::string& email, std::string& user, std::string& host) {
    if (email.empty() || checkCddbEmail(email) != CddbConfigProblem::None) return false;
    const size_t at = email.find('@');
    user = email.substr(0, at);
    host = email.substr(at + 1);
    return true;
}

CddbOptions cddbOptionsFromConfig(const CddbConfig& config, const std::string& version) {
    CddbOptions options;
    if (!config.server.empty() && checkCddbServer(config.server) == CddbConfigProblem::None)
        options.server = resolveCddbServer(config.server);
    splitCddbEmail(config.email, options.client.user, options.client.host);
    if (!version.empty() && checkCddbHelloField(version) == CddbConfigProblem::None) options.client.version = version;
    if (!config.appName.empty() && checkCddbHelloField(config.appName) == CddbConfigProblem::None)
        options.client.name = config.appName;
    if (!config.appVersion.empty() && checkCddbHelloField(config.appVersion) == CddbConfigProblem::None)
        options.client.version = config.appVersion;
    return options;
}

bool cddbServerWantsEmail(const std::string& server) {
    const std::string l = lower(server);
    const size_t start = l.find("://");
    const size_t from = start == std::string::npos ? 0 : start + 3;
    const size_t end = l.find_first_of("/?:", from);
    const std::string host = l.substr(from, end == std::string::npos ? std::string::npos : end - from);
    return host == "gnudb.org" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".gnudb.org") == 0);
}

std::string CddbTestResult::summary() const {
    std::string s = ok ? "OK: " + message : "failed: " + message;
    if (!detail.empty()) s += " (" + detail + ")";
    return s;
}

CddbTestResult testCddbConnection(HttpClient& http, const CddbOptions& options) {
    CddbTestResult result;
    try {
        const HttpResponse r = http.get(cddbRequestUrl(options.server, kCddbTestCommand, options.client));
        if (!r.ok) {
            result.message = r.error.empty() ? "HTTP request failed" : r.error;
            return result;
        }
        const std::string line = firstLine(r.body);
        result.code = responseCode(line);
        if (r.status != 200) {
            result.message = "HTTP status " + std::to_string(r.status);
            if (result.code) {
                result.message += ": " + toValidUtf8(line);
                result.hint = cddbErrorHint(line);
            }
            return result;
        }
        if (result.code == 0) {
            result.message = "not a CDDB response: " + toValidUtf8(line).substr(0, 80);
            return result;
        }
        result.message = toValidUtf8(line);
        result.ok = result.code >= 200 && result.code < 300;
        if (!result.ok) {
            result.hint = cddbErrorHint(line);
            return result;
        }
        // "Database entries: 1234567" in the stat text.
        for (const std::string& l : splitLines(r.body)) {
            const std::string t = trim(l);
            const std::string key = "Database entries:";
            if (t.rfind(key, 0) == 0) {
                const std::string n = trim(t.substr(key.size()));
                if (!n.empty() && n.find_first_not_of("0123456789") == std::string::npos)
                    result.detail = n + " database entries";
                break;
            }
        }
        result.missingEmail = options.client.anonymous() && cddbServerWantsEmail(options.server);
    } catch (const std::exception& e) {
        result = {};
        result.message = e.what();
    }
    return result;
}

}  // namespace cdr
