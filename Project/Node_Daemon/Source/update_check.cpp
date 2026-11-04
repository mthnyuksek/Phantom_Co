#include "update_check.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>

#include "http_client.hpp"
#include "version.hpp"

namespace pc2 {

namespace {
std::mutex g_mu;
bool g_checking = false, g_checked = false;
std::chrono::steady_clock::time_point g_last{};
std::string g_latest, g_url, g_error;

void check_now() {
    std::string latest, url, error;
    try {
        HttpClient cli("https://api.github.com", 10000, 20000);
        auto r = cli.Get("/repos/" PC2_RELEASES_REPO "/releases/latest",
                         {{"Accept", "application/vnd.github+json"}});  // GitHub needs a User-Agent: HttpClient sends "PhantomCo"
        if (!r) error = "GitHub could not be reached.";
        else if (r->status == 404) {}  // no release published yet: nothing newer than this one
        else if (r->status != 200) error = "GitHub answered with an error (" + std::to_string(r->status) + ").";
        else {
            json j = json::parse(r->body, nullptr, false);
            if (j.is_object() && j.contains("tag_name") && j["tag_name"].is_string()) {
                latest = j["tag_name"].get<std::string>();
                if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
                std::string u = j.contains("html_url") && j["html_url"].is_string() ? j["html_url"].get<std::string>() : "";
                // only a link on github.com is ever shown to the person
                url = u.rfind("https://github.com/", 0) == 0 ? u : "https://github.com/" PC2_RELEASES_REPO "/releases/latest";
            } else error = "GitHub's answer could not be read.";
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!latest.empty()) { g_latest = latest; g_url = url; }
    g_error = error;
    g_checked = true;
    g_checking = false;
    g_last = std::chrono::steady_clock::now();
}
}  // namespace

bool version_newer(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        int v[3] = {0, 0, 0};
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        for (int k = 0; k < 3 && i < s.size() && isdigit((unsigned char)s[i]); ++k) {
            while (i < s.size() && isdigit((unsigned char)s[i])) v[k] = std::min(v[k] * 10 + (s[i++] - '0'), 1000000);
            if (i < s.size() && s[i] == '.') ++i; else break;
        }
        return std::tuple<int, int, int>(v[0], v[1], v[2]);
    };
    return parts(a) > parts(b);
}

json update_info(bool force) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto now = std::chrono::steady_clock::now();
    bool stale = !g_checked || now - g_last > std::chrono::hours(12) || (force && now - g_last > std::chrono::seconds(10));
    if (stale && !g_checking) {
        g_checking = true;
        std::thread(check_now).detach();
    }
    return {{"current", PC2_VERSION_STR}, {"checked", g_checked}, {"checking", g_checking}, {"latest", g_latest}, {"url", g_url},
            {"newer", !g_latest.empty() && version_newer(g_latest, PC2_VERSION_STR)}, {"error", g_error}};
}

}  // namespace pc2
