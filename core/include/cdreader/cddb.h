#pragma once

#include <string>
#include <vector>

#include "cdreader/http.h"
#include "cdreader/metadata.h"
#include "cdreader/settings_store.h"
#include "cdreader/toc.h"

namespace cdr {

// CDDB (freedb protocol, as served by gnudb.org) over HTTP. Everything here is
// text processing on top of HttpClient, so it is shared by all platforms.

constexpr const char* kDefaultCddbServer = "https://gnudb.gnudb.org/~cddb/cddb.cgi";

// One disc in a query response.
struct CddbMatch {
    std::string category;  // e.g. "rock"; needed for "cddb read"
    std::string discId;    // 8 hex digits as reported by the server
    std::string title;     // "Artist / Album" as listed by the server
};

struct CddbQueryResult {
    enum class Status {
        Exact,     // 200, or 210 (several exact matches)
        Inexact,   // 211: fuzzy matches the user should pick from
        NotFound,  // 202
        Error,     // any other code, malformed response or transport failure
    };
    Status status = Status::Error;
    int code = 0;                    // CDDB response code, 0 if none
    std::vector<CddbMatch> matches;
    std::string message;             // status line or error description
};

// "cddb query <discid> <ntrks> <offset1> ... <offsetN> <nsecs>". Offsets are
// absolute frames (LBA + 150 pregap), nsecs the lead-out position in seconds.
std::string cddbQueryCommand(const Toc& toc);
std::string cddbReadCommand(const CddbMatch& match);

// application/x-www-form-urlencoded: unreserved characters stay, space -> '+'.
std::string urlEncode(const std::string& s);

// The "hello" of every request: "<user> <host> <name> <version>". Without a
// contact e-mail address configured (#38) it is anonymous (cdreader
// localhost); gnudb.org refuses that with "500 Unknown application, developer
// email for ..." and wants a real contact address as user@host.
struct CddbClientInfo {
    std::string user = "cdreader";
    std::string host = "localhost";
    std::string name = "cdreader";
    std::string version = "0.1.0";

    bool anonymous() const { return user == "cdreader" && host == "localhost"; }
};

// Full CGI URL for `command` (cmd=...&hello=...&proto=6). Spaces and control
// characters in the hello fields become '_' (the fields are space separated).
std::string cddbRequestUrl(const std::string& server, const std::string& command, const CddbClientInfo& client);

// Parses the body of a "cddb query" response.
CddbQueryResult parseCddbQueryResponse(const std::string& body);

// Parses the body of a "cddb read" response (code 210 + xmcd entry). Returns
// false and sets `error` for any other response.
bool parseCddbReadResponse(const std::string& body, AlbumMetadata& album, std::string& error);

// Parses an xmcd database entry (the lines after the 210 status line).
// TTITLEn goes to trackTitles[n]; "Artist / Title" track titles are split into
// trackArtists for compilations. Text is UTF-8; values that are not valid
// UTF-8 are taken as Latin-1.
AlbumMetadata parseXmcd(const std::string& text);

// Returns `s` if it is valid UTF-8, otherwise `s` converted from Latin-1.
std::string toValidUtf8(const std::string& s);

struct CddbOptions {
    std::string server = kDefaultCddbServer;
    size_t matchIndex = 0;   // which match to read when there are several (0-based)
    CddbClientInfo client;
};

struct CddbLookupResult {
    bool found = false;
    std::string error;                // why nothing was found (empty when found)
    std::vector<CddbMatch> matches;   // all matches from the query
    size_t chosen = 0;                // index into matches of the entry read
    bool exact = false;               // the server reported an exact match
    AlbumMetadata album;              // filled when found (discId = our TOC's id)
    // What the user can do about `error` (ContactEmail: the server refused the hello).
    enum class Hint { None, ContactEmail };
    Hint hint = Hint::None;
};

// Query + read. Never throws: network and protocol problems end up in error.
// Track titles are indexed by track number (index 0 = track 1) even when the
// disc does not start at track 1.
CddbLookupResult lookupCddb(HttpClient& http, const Toc& toc, const CddbOptions& options = {});

// rip.log block of a lookup (the same in the CLI and the Android app):
//   "CDDB lookup: disabled" /
//   "CDDB lookup (<server>): <error>" [+ "Hint: ..."] /
//   "CDDB lookup (<server>): N exact match(es)", the matches, artist, album, year, genre.
std::vector<std::string> cddbLookupLogLines(bool enabled, const std::string& server, const CddbLookupResult& result);

// --- Settings (#38) ------------------------------------------------------------

using CddbHint = CddbLookupResult::Hint;

// Hint for a CDDB server response line (e.g. the status line of a failed
// query): ContactEmail when it is a CDDB response (3 digit code) that is
// about the greeting - code 409 (no handshake) or 431 (handshake not
// successful), or a text mentioning "email", "e-mail", "mail address",
// "hello", "handshake" or "unknown application", such as gnudb's
// "500 Unknown application, developer email for cdreader 0.1.0".
// Transport errors and other server errors give None.
CddbHint cddbErrorHint(const std::string& responseLine);

// English text for rip.log / the console, starting with "Hint: " (empty for None).
std::string cddbHintText(CddbHint hint);

// What the user configured. Empty values mean the defaults.
struct CddbConfig {
    std::string server;      // CDDB CGI URL (http:// or https://); empty: kDefaultCddbServer
    std::string email;       // contact address "user@host" for the hello; empty: anonymous hello
    std::string appName;     // empty: "cdreader"
    std::string appVersion;  // empty: the program's version
};

// Keys in the settings store (also the names of 'cdreader config').
constexpr const char* kSettingCddbServer = "cddb-server";
constexpr const char* kSettingCddbEmail = "cddb-email";
constexpr const char* kSettingCddbAppName = "cddb-app-name";
constexpr const char* kSettingCddbAppVersion = "cddb-app-version";

CddbConfig cddbConfigFromSettings(const SettingsStore& store);
// Empty values remove the keys.
void storeCddbConfig(SettingsStore& store, const CddbConfig& config);

// The numbers are passed to the Android app (nativeCheckCddbSettings).
enum class CddbConfigProblem {
    None = 0,
    ServerScheme = 1,      // not http:// or https://
    ServerHost = 2,        // no host, credentials in the URL or a bad port
    ServerCharacters = 3,  // spaces, control or non-ASCII characters, '#'
    EmailShape = 4,        // not user@host.domain
    HelloCharacters = 5,   // spaces, control or non-ASCII characters, more than 64 (app name / version)
};

// Each accepts the empty string (= the default).
CddbConfigProblem checkCddbServer(const std::string& url);
CddbConfigProblem checkCddbEmail(const std::string& email);
CddbConfigProblem checkCddbHelloField(const std::string& value);
// English sentence (empty for None).
std::string describeCddbConfigProblem(CddbConfigProblem problem);
// The first problem of `config` as "<key>: <description>", empty when valid.
std::string cddbConfigError(const CddbConfig& config);

// Splits a valid address at the '@'. Returns false (and leaves the outputs
// alone) for an invalid one.
bool splitCddbEmail(const std::string& email, std::string& user, std::string& host);

// Server and hello for `config`. Values that are empty or invalid keep the
// defaults (anonymous hello, kDefaultCddbServer, "cdreader", `version`).
CddbOptions cddbOptionsFromConfig(const CddbConfig& config, const std::string& version);

// The server is gnudb.org, which needs a contact e-mail address in the hello.
bool cddbServerWantsEmail(const std::string& server);

// --- Connection test (#38) ---------------------------------------------------------
// One "stat" request (cddbd's server status: "210 OK, status information
// follows", a short text block). It needs no disc and does not touch the
// database, and it goes through the same CGI URL and hello as the lookups,
// so a server that checks the hello rejects it in the same way.
constexpr const char* kCddbTestCommand = "stat";

struct CddbTestResult {
    bool ok = false;         // a 2xx CDDB response
    int code = 0;            // CDDB response code, 0 if none
    std::string message;     // server status line, or the transport / HTTP error
    std::string detail;      // e.g. "server cddbd v1.5.2PL0, 4120000 database entries" (from the stat text), may be empty
    CddbHint hint = CddbHint::None;
    // ok, but the server is gnudb and the hello is anonymous: lookups may be refused.
    bool missingEmail = false;

    // One English line for the console.
    std::string summary() const;
};

// Never throws.
CddbTestResult testCddbConnection(HttpClient& http, const CddbOptions& options);

}  // namespace cdr
