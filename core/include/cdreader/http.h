#pragma once

#include <string>

namespace cdr {

struct HttpResponse {
    bool ok = false;     // the request completed (any HTTP status)
    int status = 0;      // HTTP status code
    std::string body;    // raw response body (may be binary)
    std::string error;   // transport error description when !ok
};

// Minimal HTTP(S) GET used by online lookups such as AccurateRip (#5) and
// CDDB (#6). Implemented per platform so that core stays portable.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(const std::string& url) = 0;
};

}  // namespace cdr
