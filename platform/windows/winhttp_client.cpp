#include "winhttp_client.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#include "spti_transport.h"

namespace cdr::win {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

HttpResponse failure(const char* step) {
    HttpResponse r;
    r.error = std::string(step) + " failed: " + win32ErrorMessage(GetLastError());
    return r;
}

}  // namespace

WinHttpClient::WinHttpClient(std::string userAgent) : userAgent_(std::move(userAgent)) {}

HttpResponse WinHttpClient::get(const std::string& url) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return failure("parsing URL");

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    Handle session(WinHttpOpen(widen(userAgent_).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return failure("WinHttpOpen");
    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection.h) return failure("connecting");
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) return failure("opening request");
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return failure("sending request");
    if (!WinHttpReceiveResponse(request.h, nullptr)) return failure("receiving response");

    HttpResponse response;
    DWORD status = 0;
    DWORD size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = int(status);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return failure("reading response");
        if (available == 0) break;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read)) return failure("reading response");
        response.body.append(chunk.data(), read);
    }
    response.ok = true;
    return response;
}

}  // namespace cdr::win
