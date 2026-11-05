#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pc2 {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResult {
    int status = 0;
    std::string body;
    std::string location;  // the Location header, if any
};

// A small HTTP(S) client on WinHTTP (http:// for the home network, https:// for a Cloudflare tunnel).
// Every call returns nullptr when the server could not be reached. One instance per thread.
class HttpClient {
public:
    // `base_url` like http://192.168.1.20:7777 or https://name.trycloudflare.com ; times in milliseconds.
    explicit HttpClient(const std::string& base_url, int connect_ms = 10000, int receive_ms = 60000, int send_ms = 60000);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    std::unique_ptr<HttpResult> Get(const std::string& path, const Headers& h = {});
    std::unique_ptr<HttpResult> Post(const std::string& path, const Headers& h, const std::string& body, const std::string& content_type);
    std::unique_ptr<HttpResult> Put(const std::string& path, const Headers& h, const std::string& body, const std::string& content_type);

private:
    std::unique_ptr<HttpResult> Request(const wchar_t* method, const std::string& path, const Headers& h, const std::string& body, const std::string& content_type);
    void* session_ = nullptr;
    void* connection_ = nullptr;
    bool secure_ = false;
};

}  // namespace pc2
