#include "http_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include "util.hpp"

namespace pc2 {

HttpClient::HttpClient(const std::string& base_url, int connect_ms, int receive_ms) {
    std::string rest = base_url;
    secure_ = rest.rfind("https://", 0) == 0;
    if (secure_) rest = rest.substr(8);
    else if (rest.rfind("http://", 0) == 0) rest = rest.substr(7);
    size_t slash = rest.find('/');
    if (slash != std::string::npos) rest.resize(slash);
    INTERNET_PORT port = secure_ ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = rest.rfind(':');
    if (colon != std::string::npos && rest.find(']') == std::string::npos) {
        port = (INTERNET_PORT)atoi(rest.c_str() + colon + 1);
        rest.resize(colon);
    }
    // Home network addresses must not go through a system proxy; the internet may need one.
    session_ = WinHttpOpen(L"PhantomCo", secure_ ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) return;
    WinHttpSetTimeouts(session_, 10000, connect_ms, 60000, receive_ms);
    connection_ = WinHttpConnect(session_, Widen(rest).c_str(), port, 0);
}

HttpClient::~HttpClient() {
    if (connection_) WinHttpCloseHandle(connection_);
    if (session_) WinHttpCloseHandle(session_);
}

std::unique_ptr<HttpResult> HttpClient::Request(const wchar_t* method, const std::string& path, const Headers& h, const std::string& body,
                                                const std::string& content_type) {
    if (!connection_) return nullptr;
    HINTERNET req = WinHttpOpenRequest(connection_, method, Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure_ ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return nullptr;
    std::wstring headers;
    for (auto& [k, v] : h) headers += Widen(k) + L": " + Widen(v) + L"\r\n";
    if (!content_type.empty()) headers += L"Content-Type: " + Widen(content_type) + L"\r\n";
    bool ok = WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    std::unique_ptr<HttpResult> out;
    if (ok) {
        out = std::make_unique<HttpResult>();
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        out->status = (int)status;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
            out->body.append(chunk, 0, got);
        }
    }
    WinHttpCloseHandle(req);
    return out;
}

std::unique_ptr<HttpResult> HttpClient::Get(const std::string& path, const Headers& h) { return Request(L"GET", path, h, "", ""); }
std::unique_ptr<HttpResult> HttpClient::Post(const std::string& path, const Headers& h, const std::string& body, const std::string& ct) {
    return Request(L"POST", path, h, body, ct);
}
std::unique_ptr<HttpResult> HttpClient::Put(const std::string& path, const Headers& h, const std::string& body, const std::string& ct) {
    return Request(L"PUT", path, h, body, ct);
}

}  // namespace pc2
