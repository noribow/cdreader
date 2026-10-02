#pragma once

#include "cdreader/http.h"

namespace cdr::win {

// HttpClient on top of WinHTTP (uses the system proxy settings).
class WinHttpClient : public HttpClient {
public:
    explicit WinHttpClient(std::string userAgent = "cdreader");
    HttpResponse get(const std::string& url) override;

private:
    std::string userAgent_;
};

}  // namespace cdr::win
