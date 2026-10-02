#pragma once

#include <string>
#include <vector>

#include "cdreader/http.h"
#include "cdreader/metadata.h"
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

struct CddbClientInfo {
    std::string user = "cdreader";    // sent in the "hello"; deliberately not the real user
    std::string host = "localhost";
    std::string name = "cdreader";
    std::string version = "0.1.0";
};

// Full CGI URL for `command` (cmd=...&hello=...&proto=6).
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
};

// Query + read. Never throws: network and protocol problems end up in error.
// Track titles are indexed by track number (index 0 = track 1) even when the
// disc does not start at track 1.
CddbLookupResult lookupCddb(HttpClient& http, const Toc& toc, const CddbOptions& options = {});

}  // namespace cdr
