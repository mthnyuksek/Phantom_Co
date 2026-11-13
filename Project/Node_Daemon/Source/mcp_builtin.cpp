#include "mcp_builtin.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>

#include "http_client.hpp"
#include "version.hpp"

namespace pc2::builtin {

// ------------------------------------------------------------------ helpers
static json text_result(const std::string& text, bool error = false) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", error}};
}

static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static std::string base64(const std::string& raw) {
    DWORD n = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string b(n, '\0');
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b.data(), &n);
    b.resize(n);
    return b;
}

static std::string form(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (auto& [k, v] : kv) out += (out.empty() ? "" : "&") + url_encode(k) + "=" + url_encode(v);
    return out;
}

// One request to a full URL (https://host/path?query).
static std::unique_ptr<HttpResult> request(const char* method, const std::string& url, const Headers& h, const std::string& body,
                                           const std::string& ct, int receive_ms = 60000, int send_ms = 60000) {
    size_t scheme = url.find("://");
    size_t slash = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
    std::string base = slash == std::string::npos ? url : url.substr(0, slash), path = slash == std::string::npos ? "/" : url.substr(slash);
    HttpClient c(base, 15000, receive_ms, send_ms);
    if (std::string(method) == "GET") return c.Get(path, h);
    if (std::string(method) == "PUT") return c.Put(path, h, body, ct);
    return c.Post(path, h, body, ct);
}

static json parse(const std::unique_ptr<HttpResult>& r) {
    if (!r) return json();
    return json::parse(r->body, nullptr, false);
}

// A file of the run's work folder ("report.pdf", "Videos/clip.mp4" or a full path inside the folder).
static bool resolve(const json& session, const std::string& p, fs::path& out, std::string& error) {
    if (p.empty()) { error = "Give the path of the file (relative to your working folder)."; return false; }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(P(session.value("workdir", "")), ec);
    fs::path f = P(p);
    if (!f.is_absolute()) f = root / f;
    f = fs::weakly_canonical(f, ec);
    const std::wstring r = root.wstring(), w = f.wstring();
    if (root.empty() || w.size() <= r.size() || _wcsnicmp(w.c_str(), r.c_str(), r.size()) != 0 || (w[r.size()] != L'\\' && w[r.size()] != L'/')) {
        error = "Only files inside your working folder can be used.";
        return false;
    }
    if (!fs::is_regular_file(f, ec)) { error = "File not found: " + p; return false; }
    out = f;
    return true;
}

static std::string lower_ext(const fs::path& f) {
    std::string e = U8(f.extension());
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

static std::string sarg(const json& a, const char* k, const std::string& def = "") {
    return a.contains(k) && a[k].is_string() ? a[k].get<std::string>() : a.contains(k) && a[k].is_number() ? a[k].dump() : def;
}

static json schema(json props, json required) { return {{"type", "object"}, {"properties", props}, {"required", required}}; }

// ----------------------------------------------------------------- telegram
static json tg_api(const std::string& token, const std::string& method, const std::string& body, const std::string& ct, std::string& error, int receive_ms = 120000) {
    // Tests point PHANTOM_TELEGRAM_API at a fake Telegram (Tests/telegram_test.py); always the real one otherwise.
    static const std::string base = [] {
        wchar_t buf[512];
        DWORD n = GetEnvironmentVariableW(L"PHANTOM_TELEGRAM_API", buf, 512);
        return n > 0 && n < 512 ? Narrow(std::wstring(buf, n)) : std::string("https://api.telegram.org");
    }();
    auto r = request("POST", base + "/bot" + token + "/" + method, {}, body, ct, receive_ms, 120000);
    json j = parse(r);
    if (!r) { error = "Telegram could not be reached."; return json(); }
    if (!j.is_object() || !j.value("ok", false)) {
        error = "Telegram: " + (j.is_object() ? j.value("description", "error " + std::to_string(r->status)) : "error " + std::to_string(r->status));
        return json();
    }
    return j["result"];
}

json telegram_api(const std::string& token, const std::string& method, const json& body, std::string& error, int receive_ms) {
    return tg_api(token, method, body.dump(), "application/json", error, receive_ms);
}

bool telegram_send(const std::string& token, const std::string& chat_id, const std::string& text, std::string& error) {
    for (size_t at = 0; at < text.size();) {  // Telegram takes at most 4096 characters per message
        std::string part = truncate_utf8(text.substr(at), 4000);
        if (part.empty()) break;
        at += part.size();
        if (tg_api(token, "sendMessage", json{{"chat_id", chat_id}, {"text", part}}.dump(), "application/json", error).is_null()) return false;
    }
    return true;
}

static json telegram_tools() {
    return json::array({
        {{"name", "send_message"}, {"description", "Send a text message through the company's Telegram bot. Without chat_id it goes to the default chat set in Phantom Co."},
         {"inputSchema", schema({{"text", {{"type", "string"}, {"description", "The message (plain text)."}}},
                                 {"chat_id", {{"type", "string"}, {"description", "Optional: chat id (number) or @channelname."}}}}, {"text"})}},
        {{"name", "send_file"}, {"description", "Send a file from your working folder through the Telegram bot (pictures are shown as photos). At most 50 MB."},
         {"inputSchema", schema({{"path", {{"type", "string"}, {"description", "File path relative to your working folder."}}},
                                 {"caption", {{"type", "string"}, {"description", "Optional text under the file."}}},
                                 {"chat_id", {{"type", "string"}, {"description", "Optional: chat id or @channelname."}}}}, {"path"})}},
        {{"name", "read_messages"}, {"description", "Read the latest messages people sent to the bot (from the last 24 hours), with their chat ids."},
         {"inputSchema", schema({{"limit", {{"type", "integer"}, {"description", "How many (1-50, default 20)."}}}}, json::array())}},
    });
}

static json telegram_call(const std::string& tool, const json& a, const json& session) {
    const std::string token = session["secrets"].value("bot_token", "");
    if (token.empty()) return text_result("The Telegram bot has no token yet: the owner adds it in Settings > MCP > Telegram.", true);
    std::string chat = sarg(a, "chat_id", session["options"].value("chat_id", "")), error;
    if (tool == "send_message") {
        std::string text = sarg(a, "text");
        if (text.empty()) return text_result("Give the text of the message.", true);
        if (chat.empty()) return text_result("No chat id: give chat_id, or the owner sets a default chat in Settings > MCP > Telegram. read_messages shows the ids of people who wrote to the bot.", true);
        int sent = 0;
        for (size_t at = 0; at < text.size();) {  // Telegram takes at most 4096 characters per message
            std::string part = truncate_utf8(text.substr(at), 4000);
            at += part.size();
            if (part.empty()) break;
            if (tg_api(token, "sendMessage", json{{"chat_id", chat}, {"text", part}}.dump(), "application/json", error).is_null()) return text_result(error, true);
            ++sent;
        }
        return text_result(sent > 1 ? "Sent in " + std::to_string(sent) + " messages." : "Sent.");
    }
    if (tool == "send_file") {
        if (chat.empty()) return text_result("No chat id: give chat_id, or the owner sets a default chat in Settings > MCP > Telegram.", true);
        fs::path f;
        if (!resolve(session, sarg(a, "path"), f, error)) return text_result(error, true);
        std::error_code ec;
        if (fs::file_size(f, ec) > 50ull * 1024 * 1024) return text_result("The file is larger than 50 MB (Telegram's limit for bots).", true);
        std::string data;
        if (!read_file(f, data)) return text_result("Could not read the file.", true);
        const std::string ext = lower_ext(f);
        const bool photo = (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp") && data.size() < 10 * 1024 * 1024;
        const std::string boundary = "----PhantomCo" + std::to_string(GetTickCount64());
        std::string name = U8(f.filename());
        for (auto& c : name) if (c == '"' || c == '\r' || c == '\n') c = '_';
        std::string body;
        auto field = [&](const std::string& k, const std::string& v) {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + k + "\"\r\n\r\n" + v + "\r\n";
        };
        field("chat_id", chat);
        if (!sarg(a, "caption").empty()) field("caption", truncate_utf8(sarg(a, "caption"), 1000));
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + std::string(photo ? "photo" : "document") + "\"; filename=\"" + name +
                "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n--" + boundary + "--\r\n";
        if (tg_api(token, photo ? "sendPhoto" : "sendDocument", body, "multipart/form-data; boundary=" + boundary, error).is_null()) return text_result(error, true);
        return text_result("Sent " + name + ".");
    }
    if (tool == "read_messages") {
        int limit = a.contains("limit") && a["limit"].is_number_integer() ? std::clamp(a["limit"].get<int>(), 1, 50) : 20;
        json ups = tg_api(token, "getUpdates", json{{"timeout", 0}, {"limit", 100}}.dump(), "application/json", error);
        if (ups.is_null()) return text_result(error, true);
        std::vector<std::string> lines;
        for (auto& u : ups) {
            const json& m = u.contains("message") ? u["message"] : u.contains("channel_post") ? u["channel_post"] : json();
            if (!m.is_object()) continue;
            const json& c = m.value("chat", json::object());
            const json& fr = m.value("from", json::object());
            std::time_t t = (std::time_t)m.value("date", 0LL);
            char when[32] = "";
            std::tm tmv{};
            if (localtime_s(&tmv, &t) == 0) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
            std::string chat_name = c.value("title", c.value("first_name", "") + (c.contains("last_name") ? " " + c.value("last_name", "") : ""));
            std::string from = fr.value("first_name", "") + (fr.contains("username") ? " (@" + fr.value("username", "") + ")" : "");
            lines.push_back(std::string(when) + " · chat " + std::to_string(c.value("id", 0LL)) + " (" + chat_name + ")" + (from.empty() ? "" : " · " + from) + ": " +
                            m.value("text", m.value("caption", "[" + std::string(m.contains("photo") ? "photo" : m.contains("document") ? "file" : "other") + "]")));
        }
        if (lines.empty()) return text_result("No new messages in the last 24 hours. (People must write to the bot first; send /start to it.)");
        std::string out;
        for (size_t i = lines.size() > (size_t)limit ? lines.size() - limit : 0; i < lines.size(); ++i) out += lines[i] + "\n";
        return text_result(out);
    }
    return text_result("Unknown tool: " + tool, true);
}

// ------------------------------------------------------------------ youtube
static const char* kScopes = "https://www.googleapis.com/auth/youtube.upload https://www.googleapis.com/auth/youtube.readonly";

std::string pkce_challenge(const std::string& verifier) {
    std::string hex = sha256_hex_of(verifier), raw;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) raw += (char)std::stoi(hex.substr(i, 2), nullptr, 16);
    std::string out;
    for (char c : base64(raw)) { if (c == '+') out += '-'; else if (c == '/') out += '_'; else if (c != '=' && c != '\0') out += c; }
    return out;
}

std::string google_auth_url(const std::string& client_id, const std::string& redirect, const std::string& state, const std::string& challenge) {
    return "https://accounts.google.com/o/oauth2/v2/auth?" +
           form({{"client_id", client_id}, {"redirect_uri", redirect}, {"response_type", "code"}, {"scope", kScopes}, {"access_type", "offline"},
                 {"prompt", "consent"}, {"state", state}, {"code_challenge", challenge}, {"code_challenge_method", "S256"}});
}

static std::string google_error(const json& j, int status) {
    if (j.is_object() && j.contains("error")) {
        if (j["error"].is_string()) return j["error"].get<std::string>() + (j.contains("error_description") ? ": " + j.value("error_description", "") : "");
        if (j["error"].is_object()) return j["error"].value("message", "error " + std::to_string(status));
    }
    return "error " + std::to_string(status);
}

bool google_exchange(const std::string& client_id, const std::string& client_secret, const std::string& code, const std::string& verifier,
                     const std::string& redirect, std::string& refresh_token, std::string& error) {
    auto r = request("POST", "https://oauth2.googleapis.com/token", {},
                     form({{"code", code}, {"client_id", client_id}, {"client_secret", client_secret}, {"redirect_uri", redirect},
                           {"grant_type", "authorization_code"}, {"code_verifier", verifier}}), "application/x-www-form-urlencoded");
    json j = parse(r);
    if (!r) { error = "Google could not be reached."; return false; }
    if (r->status != 200 || !j.is_object() || j.value("refresh_token", "").empty()) { error = "Google: " + google_error(j, r->status); return false; }
    refresh_token = j["refresh_token"];
    return true;
}

static std::string google_access(const json& secrets, std::string& error) {
    if (secrets.value("refresh_token", "").empty()) { error = "YouTube is not connected: the owner signs in with Google in Settings > MCP > YouTube."; return ""; }
    auto r = request("POST", "https://oauth2.googleapis.com/token", {},
                     form({{"client_id", secrets.value("client_id", "")}, {"client_secret", secrets.value("client_secret", "")},
                           {"refresh_token", secrets.value("refresh_token", "")}, {"grant_type", "refresh_token"}}), "application/x-www-form-urlencoded");
    json j = parse(r);
    if (!r) { error = "Google could not be reached."; return ""; }
    if (r->status != 200 || !j.is_object() || j.value("access_token", "").empty()) {
        error = "Google refused the sign-in (" + google_error(j, r->status) + "). Sign in again in Settings > MCP > YouTube.";
        return "";
    }
    return j["access_token"];
}

static json yt_get(const std::string& token, const std::string& path, std::string& error) {
    auto r = request("GET", "https://www.googleapis.com/youtube/v3/" + path, {{"Authorization", "Bearer " + token}}, "", "");
    json j = parse(r);
    if (!r) { error = "YouTube could not be reached."; return json(); }
    if (r->status != 200) { error = "YouTube: " + google_error(j, r->status); return json(); }
    return j;
}

static json youtube_tools() {
    return json::array({
        {{"name", "upload_video"}, {"description", "Upload a video file from your working folder to the company's YouTube channel. It is private unless privacy says otherwise. Returns the link."},
         {"inputSchema", schema({{"path", {{"type", "string"}, {"description", "Video file path relative to your working folder (mp4, mov, webm...)."}}},
                                 {"title", {{"type", "string"}, {"description", "Title, at most 100 characters."}}},
                                 {"description", {{"type", "string"}, {"description", "Description, at most 5000 characters."}}},
                                 {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional tags."}}},
                                 {"privacy", {{"type", "string"}, {"enum", {"private", "unlisted", "public"}}, {"description", "Default private."}}},
                                 {"made_for_kids", {{"type", "boolean"}, {"description", "Is the video made for children? Default false."}}}},
                                {"path", "title"})}},
        {{"name", "set_thumbnail"}, {"description", "Set the thumbnail picture (JPG or PNG, at most 2 MB) of one of the channel's videos."},
         {"inputSchema", schema({{"video_id", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Image path relative to your working folder."}}}},
                                {"video_id", "path"})}},
        {{"name", "list_my_videos"}, {"description", "List the latest videos of the company's YouTube channel (title, id, date, privacy)."},
         {"inputSchema", schema({{"limit", {{"type", "integer"}, {"description", "How many (1-50, default 10)."}}}}, json::array())}},
    });
}

static json youtube_call(const std::string& tool, const json& a, const json& session) {
    std::string error;
    const json& sec = session["secrets"];
    std::string token = google_access(sec, error);
    if (token.empty()) return text_result(error, true);
    if (tool == "upload_video") {
        fs::path f;
        if (!resolve(session, sarg(a, "path"), f, error)) return text_result(error, true);
        std::error_code ec;
        const uintmax_t size = fs::file_size(f, ec);
        if (size > 2000ull * 1024 * 1024) return text_result("The video is larger than 2 GB; Phantom Co. cannot upload files that big yet.", true);
        std::string title = truncate_utf8(sarg(a, "title"), 100);
        if (title.empty()) return text_result("Give the video a title.", true);
        std::string privacy = sarg(a, "privacy", "private");
        if (privacy != "private" && privacy != "unlisted" && privacy != "public") privacy = "private";
        json tags = json::array();
        if (a.contains("tags") && a["tags"].is_array()) for (auto& t : a["tags"]) if (t.is_string() && tags.size() < 30) tags.push_back(truncate_utf8(t.get<std::string>(), 60));
        json meta = {{"snippet", {{"title", title}, {"description", truncate_utf8(sarg(a, "description"), 5000)}, {"tags", tags}, {"categoryId", "22"}}},
                     {"status", {{"privacyStatus", privacy}, {"selfDeclaredMadeForKids", a.value("made_for_kids", false)}}}};
        auto start = request("POST", "https://www.googleapis.com/upload/youtube/v3/videos?uploadType=resumable&part=snippet,status",
                             {{"Authorization", "Bearer " + token}, {"X-Upload-Content-Type", "video/*"}, {"X-Upload-Content-Length", std::to_string(size)}},
                             meta.dump(), "application/json; charset=UTF-8");
        if (!start) return text_result("YouTube could not be reached.", true);
        if (start->status != 200 || start->location.empty()) return text_result("YouTube refused the upload: " + google_error(parse(start), start->status), true);
        std::string data;
        if (!read_file(f, data)) return text_result("Could not read the video file.", true);
        auto up = request("PUT", start->location, {{"Authorization", "Bearer " + token}}, data, "video/*", 60 * 60 * 1000, 60 * 60 * 1000);
        json v = parse(up);
        if (!up) return text_result("The upload was cut off (no answer from YouTube).", true);
        if ((up->status != 200 && up->status != 201) || !v.is_object() || v.value("id", "").empty())
            return text_result("The upload failed: " + google_error(v, up->status), true);
        return text_result("Uploaded as " + privacy + ": https://youtu.be/" + v.value("id", "") + " (video id " + v.value("id", "") + ")." +
                           (privacy != "private" ? "" : " It is private: only the channel owner sees it until it is made public in YouTube Studio."));
    }
    if (tool == "set_thumbnail") {
        const std::string vid = sarg(a, "video_id");
        if (vid.empty() || vid.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            return text_result("Give a valid video id.", true);
        fs::path f;
        if (!resolve(session, sarg(a, "path"), f, error)) return text_result(error, true);
        const std::string ext = lower_ext(f);
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") return text_result("The thumbnail must be a JPG or PNG picture.", true);
        std::string data;
        if (!read_file(f, data) || data.size() > 2 * 1024 * 1024) return text_result("Could not read the picture, or it is larger than 2 MB.", true);
        auto r = request("POST", "https://www.googleapis.com/upload/youtube/v3/thumbnails/set?videoId=" + vid, {{"Authorization", "Bearer " + token}}, data,
                         ext == ".png" ? "image/png" : "image/jpeg", 300000, 300000);
        if (!r) return text_result("YouTube could not be reached.", true);
        if (r->status != 200) return text_result("YouTube refused the thumbnail: " + google_error(parse(r), r->status), true);
        return text_result("Thumbnail set.");
    }
    if (tool == "list_my_videos") {
        int limit = a.contains("limit") && a["limit"].is_number_integer() ? std::clamp(a["limit"].get<int>(), 1, 50) : 10;
        json ch = yt_get(token, "channels?part=contentDetails,snippet&mine=true", error);
        if (ch.is_null()) return text_result(error, true);
        if (!ch.contains("items") || ch["items"].empty()) return text_result("This Google account has no YouTube channel yet.", true);
        const json& c = ch["items"][0];
        std::string uploads = c["contentDetails"]["relatedPlaylists"].value("uploads", "");
        json items = yt_get(token, "playlistItems?part=snippet,status&maxResults=" + std::to_string(limit) + "&playlistId=" + uploads, error);
        if (items.is_null()) return text_result(error, true);
        std::string out = "Channel: " + c["snippet"].value("title", "") + "\n";
        for (auto& it : items.value("items", json::array())) {
            const json& s = it["snippet"];
            out += "- " + s.value("title", "") + " · https://youtu.be/" + s["resourceId"].value("videoId", "") + " · " + s.value("publishedAt", "").substr(0, 10) + " · " +
                   it.value("status", json::object()).value("privacyStatus", "") + "\n";
        }
        if (items.value("items", json::array()).empty()) out += "(no videos yet)\n";
        return text_result(out);
    }
    return text_result("Unknown tool: " + tool, true);
}

// ------------------------------------------------------------------- reddit
// With an app Reddit has approved (client id + secret, plus user name + password of a "script" app for posting) the
// official API is used. Without one only Reddit's public RSS feeds can be read: its logged-out JSON answers 403.
static std::string reddit_host(bool api) {
    // Tests point PHANTOM_REDDIT_API at a fake Reddit (Tests/reddit_test.py); always the real one otherwise.
    static const std::string test = [] {
        wchar_t buf[512];
        DWORD n = GetEnvironmentVariableW(L"PHANTOM_REDDIT_API", buf, 512);
        return n > 0 && n < 512 ? Narrow(std::wstring(buf, n)) : std::string();
    }();
    return !test.empty() ? test : api ? "https://oauth.reddit.com" : "https://www.reddit.com";
}

static Headers reddit_headers(const json& sec, const std::string& token = "") {
    std::string ua = "windows:phantom_co:v" PC2_VERSION_STR;
    if (!sec.value("username", "").empty()) ua += " (by /u/" + sec.value("username", "") + ")";
    Headers h{{"User-Agent", ua}};
    if (!token.empty()) h.push_back({"Authorization", "Bearer " + token});
    return h;
}

static std::string reddit_status_text(int status) {
    if (status == 401) return "Reddit no longer accepts this sign-in. Check the app and the account in Settings > MCP > Reddit.";
    if (status == 403) return "Reddit refused this (a private or banned community, or the app has no permission).";
    if (status == 404) return "Reddit does not know that community or post.";
    if (status == 429) return "Reddit is limiting requests right now. Try again in a few minutes.";
    return "Reddit answered with error " + std::to_string(status) + ".";
}

// "" with no error = no app saved, so public feeds only.
static std::string reddit_token(const json& sec, std::string& error) {
    const std::string id = sec.value("client_id", ""), secret = sec.value("client_secret", ""), user = sec.value("username", ""), pass = sec.value("password", "");
    if (id.empty() || secret.empty()) return "";
    static std::mutex mu;
    static std::string cached, cached_for;
    static ULONGLONG until = 0;
    const std::string who = id + "\n" + secret + "\n" + user + "\n" + pass;
    std::lock_guard<std::mutex> l(mu);
    if (!cached.empty() && cached_for == who && GetTickCount64() < until) return cached;
    Headers h = reddit_headers(sec);
    h.push_back({"Authorization", "Basic " + base64(id + ":" + secret)});
    const std::string body = !user.empty() && !pass.empty() ? form({{"grant_type", "password"}, {"username", user}, {"password", pass}}) : form({{"grant_type", "client_credentials"}});
    auto r = request("POST", reddit_host(false) + "/api/v1/access_token", h, body, "application/x-www-form-urlencoded");
    json j = parse(r);
    if (!r) { error = "Reddit could not be reached."; return ""; }
    if (r->status == 401 || r->status == 403) { error = "Reddit refused this app: the client id or secret is wrong, or Reddit has not approved the app."; return ""; }
    if (j.is_object() && j.value("error", "") == "invalid_grant") { error = "Reddit refused the user name or password (an account with two-factor sign-in cannot be used here)."; return ""; }
    if (r->status != 200 || !j.is_object() || j.value("access_token", "").empty()) { error = reddit_status_text(r->status); return ""; }
    cached = j["access_token"];
    cached_for = who;
    until = GetTickCount64() + std::max(60, j.value("expires_in", 3600) - 60) * 1000ULL;
    return cached;
}

static std::string xml_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        size_t e = s[i] == '&' ? s.find(';', i) : std::string::npos;
        if (e == std::string::npos || e - i > 10) { out += s[i]; continue; }
        const std::string name = s.substr(i + 1, e - i - 1);
        unsigned long c = 0;
        if (name == "amp") c = '&';
        else if (name == "lt") c = '<';
        else if (name == "gt") c = '>';
        else if (name == "quot") c = '"';
        else if (name == "apos") c = '\'';
        else if (name.size() > 2 && name[0] == '#' && (name[1] == 'x' || name[1] == 'X')) c = strtoul(name.c_str() + 2, nullptr, 16);
        else if (name.size() > 1 && name[0] == '#') c = strtoul(name.c_str() + 1, nullptr, 10);
        if (c == 0 || c > 0x10FFFF) { out += s[i]; continue; }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
        i = e;
    }
    return out;
}

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The text of a feed entry: an escaped piece of HTML; Reddit escapes entities in it once more (&amp;#32;).
static std::string feed_text(const std::string& html_escaped) {
    const std::string html = xml_decode(html_escaped);
    std::string plain;
    for (size_t i = 0; i < html.size(); ++i) {
        if (html[i] != '<') { plain += html[i]; continue; }
        size_t e = html.find('>', i);
        if (e == std::string::npos) break;
        const std::string tag = html.substr(i + 1, std::min<size_t>(e - i - 1, 4));
        if (tag.rfind("br", 0) == 0 || tag.rfind("/p", 0) == 0 || tag.rfind("/li", 0) == 0) plain += '\n';
        i = e;
    }
    plain = xml_decode(plain);
    size_t tail = plain.rfind("submitted by");  // the feed's own footer: author and [link] [comments]
    if (tail != std::string::npos) plain.erase(tail);
    return trim(plain);
}

struct FeedEntry { std::string title, author, sub, id, link, date, text; };

static std::string between(const std::string& s, const std::string& open, const std::string& close, size_t from = 0) {
    size_t a = s.find(open, from);
    if (a == std::string::npos) return "";
    a += open.size();
    size_t b = s.find(close, a);
    return b == std::string::npos ? "" : s.substr(a, b - a);
}

static std::vector<FeedEntry> feed_entries(const std::string& body) {
    std::vector<FeedEntry> out;
    for (size_t at = body.find("<entry>"); at != std::string::npos; at = body.find("<entry>", at + 1)) {
        size_t end = body.find("</entry>", at);
        if (end == std::string::npos) break;
        const std::string e = body.substr(at, end - at);
        FeedEntry f;
        f.title = xml_decode(xml_decode(between(e, "<title>", "</title>")));  // escaped twice, like the content
        f.author = xml_decode(between(e, "<name>", "</name>"));
        if (f.author.rfind("/u/", 0) == 0) f.author.erase(0, 1);
        f.sub = xml_decode(between(e, "<category term=\"", "\""));
        f.id = between(e, "<id>", "</id>");
        f.link = xml_decode(between(e, "<link href=\"", "\""));
        f.date = between(e, "<updated>", "</updated>").substr(0, 10);
        f.text = feed_text(between(e, "<content type=\"html\">", "</content>"));
        out.push_back(f);
    }
    return out;
}

static bool feed_get(const json& sec, const std::string& path, std::string& body, std::string& error) {
    auto r = request("GET", reddit_host(false) + path, reddit_headers(sec), "", "");
    if (!r) { error = "Reddit could not be reached."; return false; }
    if (r->status == 429) { error = "Reddit limits reading without an approved app. Try again in a few minutes."; return false; }
    if (r->status != 200) { error = reddit_status_text(r->status); return false; }
    body = r->body;
    return true;
}

static json reddit_get(const json& sec, const std::string& token, const std::string& path, std::string& error) {
    auto r = request("GET", reddit_host(true) + path, reddit_headers(sec, token), "", "");
    json j = parse(r);
    if (!r) { error = "Reddit could not be reached."; return json(); }
    if (r->status != 200 || j.is_discarded()) { error = reddit_status_text(r->status); return json(); }
    return j;
}

static bool reddit_sub(std::string s, std::string& out) {
    if (s.rfind("/r/", 0) == 0) s.erase(0, 3);
    else if (s.rfind("r/", 0) == 0) s.erase(0, 2);
    if (s.size() < 2 || s.size() > 21 || s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) return false;
    out = s;
    return true;
}

// A post or comment as "t3_abc" / "t1_abc" from a full name, a bare post id or a link to the post.
static bool reddit_thing(std::string s, std::string& out) {
    size_t c = s.find("/comments/");
    if (c != std::string::npos) { s = s.substr(c + 10); s = "t3_" + s.substr(0, s.find_first_of("/?#")); }
    else if (s.rfind("t1_", 0) != 0 && s.rfind("t3_", 0) != 0) s = "t3_" + s;
    if (s.size() < 4 || s.size() > 20 || s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789", 3) != std::string::npos) return false;
    out = s;
    return true;
}

static std::string feed_line(const FeedEntry& f, size_t max_text) {
    std::string out = "- " + f.title + "\n  " + (f.sub.empty() ? "" : "r/" + f.sub + " · ") + f.author + " · " + f.date + " · id " + f.id + " · " + f.link + "\n";
    if (!f.text.empty()) out += "  " + truncate_utf8(f.text, max_text) + "\n";
    return out;
}

static std::string post_line(const json& d, size_t max_text) {
    std::time_t t = (std::time_t)d.value("created_utc", 0.0);
    char day[16] = "";
    std::tm tmv{};
    if (gmtime_s(&tmv, &t) == 0) std::strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
    std::string out = "- " + d.value("title", "") + "\n  r/" + d.value("subreddit", "") + " · u/" + d.value("author", "") + " · " + day + " · " +
                      std::to_string(d.value("score", 0)) + " points · " + std::to_string(d.value("num_comments", 0)) + " comments · id " + d.value("name", "") +
                      " · https://www.reddit.com" + d.value("permalink", "") + "\n";
    if (!d.value("is_self", true)) out += "  link: " + d.value("url", "") + "\n";
    if (!d.value("selftext", "").empty()) out += "  " + truncate_utf8(d.value("selftext", ""), max_text) + "\n";
    return out;
}

static void comment_lines(const json& listing, int depth, std::string& out) {
    if (!listing.is_object() || !listing.contains("data") || depth > 4) return;
    for (const json& c : listing["data"].value("children", json::array())) {
        if (c.value("kind", "") != "t1") continue;
        const json d = c.value("data", json::object());
        out += std::string(depth * 2, ' ') + "- u/" + d.value("author", "") + " (" + std::to_string(d.value("score", 0)) + ", id " + d.value("name", "") + "): " +
               truncate_utf8(d.value("body", ""), 1500) + "\n";
        if (d.contains("replies")) comment_lines(d["replies"], depth + 1, out);
    }
}

static json reddit_tools() {
    return json::array({
        {{"name", "list_posts"}, {"description", "List the posts of a subreddit."},
         {"inputSchema", schema({{"subreddit", {{"type", "string"}, {"description", "Name of the community, e.g. programming."}}},
                                 {"sort", {{"type", "string"}, {"enum", {"hot", "new", "top", "rising"}}, {"description", "Default hot."}}},
                                 {"limit", {{"type", "integer"}, {"description", "How many (1-50, default 15)."}}}}, {"subreddit"})}},
        {{"name", "search"}, {"description", "Search Reddit, or one subreddit, for posts."},
         {"inputSchema", schema({{"query", {{"type", "string"}}},
                                 {"subreddit", {{"type", "string"}, {"description", "Optional: search only this community."}}},
                                 {"sort", {{"type", "string"}, {"enum", {"relevance", "new", "top", "comments"}}, {"description", "Default relevance."}}},
                                 {"limit", {{"type", "integer"}, {"description", "How many (1-50, default 15)."}}}}, {"query"})}},
        {{"name", "read_post"}, {"description", "Read one post with its comments."},
         {"inputSchema", schema({{"post", {{"type", "string"}, {"description", "The post's id (t3_abc) or its link."}}},
                                 {"limit", {{"type", "integer"}, {"description", "How many comments (1-100, default 30)."}}}}, {"post"})}},
        {{"name", "submit_post"}, {"description", "Publish a post in a subreddit with the company's Reddit account. Give text for a text post or url for a link post."},
         {"inputSchema", schema({{"subreddit", {{"type", "string"}}}, {"title", {{"type", "string"}, {"description", "At most 300 characters."}}},
                                 {"text", {{"type", "string"}}}, {"url", {{"type", "string"}}}}, {"subreddit", "title"})}},
        {{"name", "comment"}, {"description", "Reply to a post or a comment with the company's Reddit account."},
         {"inputSchema", schema({{"parent", {{"type", "string"}, {"description", "Id of the post (t3_abc) or comment (t1_abc), or the post's link."}}},
                                 {"text", {{"type", "string"}}}}, {"parent", "text"})}},
    });
}

static bool reddit_write(const json& sec, const std::string& token, const std::string& path, const std::string& body, json& data, std::string& error) {
    auto r = request("POST", reddit_host(true) + path, reddit_headers(sec, token), body, "application/x-www-form-urlencoded");
    json j = parse(r);
    if (!r) { error = "Reddit could not be reached, so it is not known whether this went through."; return false; }
    if (r->status != 200 || !j.is_object() || !j.contains("json")) { error = reddit_status_text(r->status); return false; }
    const json errors = j["json"].value("errors", json::array());
    if (!errors.empty()) {
        error = "Reddit refused this: ";
        for (const json& e : errors) if (e.is_array() && e.size() > 1 && e[1].is_string()) error += e[1].get<std::string>() + " ";
        return false;
    }
    data = j["json"].value("data", json::object());
    return true;
}

static json reddit_call(const std::string& tool, const json& a, const json& session) {
    const json& sec = session["secrets"];
    std::string error, sub;
    const std::string token = reddit_token(sec, error);
    if (!error.empty()) return text_result(error, true);
    const int limit = a.contains("limit") && a["limit"].is_number_integer() ? std::clamp(a["limit"].get<int>(), 1, tool == "read_post" ? 100 : 50) : (tool == "read_post" ? 30 : 15);
    const std::string n = std::to_string(limit);

    if (tool == "list_posts") {
        if (!reddit_sub(sarg(a, "subreddit"), sub)) return text_result("Give the name of a subreddit, e.g. programming.", true);
        std::string sort = sarg(a, "sort", "hot");
        if (sort != "hot" && sort != "new" && sort != "top" && sort != "rising") sort = "hot";
        std::string out;
        if (token.empty()) {
            std::string body;
            if (!feed_get(sec, "/r/" + sub + "/" + sort + ".rss?limit=" + n, body, error)) return text_result(error, true);
            for (const FeedEntry& f : feed_entries(body)) out += feed_line(f, 400);
        } else {
            json j = reddit_get(sec, token, "/r/" + sub + "/" + sort + "?raw_json=1&limit=" + n, error);
            if (j.is_null()) return text_result(error, true);
            for (const json& c : j["data"].value("children", json::array())) out += post_line(c.value("data", json::object()), 400);
        }
        return text_result(out.empty() ? "No posts." : out);
    }
    if (tool == "search") {
        const std::string q = sarg(a, "query");
        if (q.empty()) return text_result("Give something to search for.", true);
        if (!sarg(a, "subreddit").empty() && !reddit_sub(sarg(a, "subreddit"), sub)) return text_result("That is not the name of a subreddit.", true);
        std::string sort = sarg(a, "sort", "relevance");
        if (sort != "new" && sort != "top" && sort != "comments") sort = "relevance";
        const std::string where = sub.empty() ? "/search" : "/r/" + sub + "/search";
        const std::string query = "?q=" + url_encode(q) + "&sort=" + sort + "&limit=" + n + (sub.empty() ? "" : "&restrict_sr=on");
        std::string out;
        if (token.empty()) {
            std::string body;
            if (!feed_get(sec, where + ".rss" + query, body, error)) return text_result(error, true);
            for (const FeedEntry& f : feed_entries(body)) out += feed_line(f, 400);
        } else {
            json j = reddit_get(sec, token, where + query + "&raw_json=1", error);
            if (j.is_null()) return text_result(error, true);
            for (const json& c : j["data"].value("children", json::array())) out += post_line(c.value("data", json::object()), 400);
        }
        return text_result(out.empty() ? "Nothing found." : out);
    }
    if (tool == "read_post") {
        std::string thing;
        if (!reddit_thing(sarg(a, "post"), thing) || thing.rfind("t3_", 0) != 0) return text_result("Give the post's id (t3_abc) or its link.", true);
        const std::string id = thing.substr(3);
        std::string out;
        if (token.empty()) {
            std::string body;
            if (!feed_get(sec, "/comments/" + id + ".rss?limit=" + n, body, error)) return text_result(error, true);
            bool first = true;
            for (const FeedEntry& f : feed_entries(body)) {
                out += first ? feed_line(f, 8000) + "\nComments:\n" : "- " + f.author + " (id " + f.id + "): " + truncate_utf8(f.text, 1500) + "\n";
                first = false;
            }
        } else {
            json j = reddit_get(sec, token, "/comments/" + id + "?raw_json=1&depth=4&limit=" + n, error);
            if (j.is_null()) return text_result(error, true);
            if (!j.is_array() || j.size() < 2) return text_result("Reddit sent something unexpected.", true);
            for (const json& c : j[0]["data"].value("children", json::array())) out += post_line(c.value("data", json::object()), 8000);
            out += "\nComments:\n";
            comment_lines(j[1], 0, out);
        }
        return text_result(out.empty() ? "Post not found." : out);
    }
    if (tool == "submit_post" || tool == "comment") {
        if (token.empty() || sec.value("username", "").empty() || sec.value("password", "").empty())
            return text_result("Posting needs an app approved by Reddit and an account: the owner adds them in Settings > MCP > Reddit. Without them Reddit can only be read.", true);
        json data;
        if (tool == "comment") {
            std::string thing;
            const std::string text = sarg(a, "text");
            if (text.empty() || !reddit_thing(sarg(a, "parent"), thing)) return text_result("Give the post or comment to answer and the text.", true);
            if (!reddit_write(sec, token, "/api/comment", form({{"api_type", "json"}, {"thing_id", thing}, {"text", truncate_utf8(text, 10000)}}), data, error)) return text_result(error, true);
            return text_result("Comment published.");
        }
        if (!reddit_sub(sarg(a, "subreddit"), sub)) return text_result("Give the name of a subreddit.", true);
        const std::string title = truncate_utf8(sarg(a, "title"), 300), text = sarg(a, "text"), url = sarg(a, "url");
        if (title.empty()) return text_result("Give the post a title.", true);
        if (text.empty() == url.empty()) return text_result("Give either text (a text post) or url (a link post).", true);
        std::vector<std::pair<std::string, std::string>> kv = {{"api_type", "json"}, {"sr", sub}, {"title", title}, {"kind", url.empty() ? "self" : "link"}};
        if (url.empty()) kv.push_back({"text", truncate_utf8(text, 40000)});
        else kv.push_back({"url", url});
        if (!reddit_write(sec, token, "/api/submit", form(kv), data, error)) return text_result(error, true);
        return text_result("Published: " + data.value("url", "(no link came back)"));
    }
    return text_result("Unknown tool: " + tool, true);
}

static json reddit_check(const json& sec) {
    std::string error;
    const std::string token = reddit_token(sec, error);
    if (!error.empty()) return {{"ok", false}, {"text", error}};
    if (token.empty()) {
        std::string body;
        if (!feed_get(sec, "/r/popular/hot.rss?limit=1", body, error) && error.find("limits reading") == std::string::npos) return {{"ok", false}, {"text", error}};
        return {{"ok", true}, {"text", "Read only: public feeds, no account"}};
    }
    if (sec.value("username", "").empty() || sec.value("password", "").empty()) return {{"ok", true}, {"text", "Read only: app without an account"}};
    json me = reddit_get(sec, token, "/api/v1/me", error);
    if (me.is_null()) return {{"ok", false}, {"text", error}};
    return {{"ok", true}, {"text", "u/" + me.value("name", "")}};
}

// ---------------------------------------------------------------- reminders
// The user's own Cloudflare Worker (Project/Cloudflare_Worker) keeps the reminders and sends them to Telegram at the
// time, also while this PC is off. Phantom Co. only talks to it: POST /reminders, GET /reminders?chat=, POST
// /reminders/cancel, GET /health (Bearer worker_key). A reminder goes to the Telegram of the person the ghost works for.
static std::string worker_base(const json& sec) {
    std::string u = sec.value("worker_url", "");
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    return u;
}

static Headers worker_headers(const json& sec) { return {{"Authorization", "Bearer " + sec.value("worker_key", "")}}; }

static std::string worker_error(const std::unique_ptr<HttpResult>& r, const json& j) {
    if (!r) return "The reminder Worker could not be reached. Check its address in Settings > MCP > Reminders.";
    if (r->status == 401 || r->status == 403) return "The Worker refused the key. Check the key in Settings > MCP > Reminders.";
    if (j.is_object() && j.contains("error") && j["error"].is_string()) return "Worker: " + j["error"].get<std::string>();
    return "The Worker answered with error " + std::to_string(r->status) + ".";
}

// "2026-11-16 16:00" (this PC's local time) <-> unix seconds
static bool parse_local(const std::string& s, long long& out) {
    int y, mo, d, h, mi;
    std::string t5 = s;  // "2026-11-16T16:00:00" is accepted too; seconds are ignored
    for (auto& c : t5) if (c == 'T') c = ' ';
    if (sscanf(t5.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return false;
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
    std::time_t v = std::mktime(&t);
    if (v == (std::time_t)-1 || t.tm_mday != d || t.tm_mon != mo - 1) return false;  // 31 November and the like
    out = (long long)v;
    return true;
}

static std::string local_text(long long unix_s) {
    std::time_t t = (std::time_t)unix_s;
    std::tm tmv{};
    char buf[32] = "";
    if (localtime_s(&tmv, &t) == 0) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

static json reminders_tools() {
    // The proxy asks for the tool list at the start of every run, so the ghost always knows the date (its prompt has none).
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    char now[64] = "";
    if (localtime_s(&tmv, &t) == 0) std::strftime(now, sizeof(now), "%A %Y-%m-%d %H:%M", &tmv);
    return json::array({
        {{"name", "schedule_reminders"}, {"description", std::string("Schedule Telegram reminders for the person you work for. Each one is sent to their Telegram at the given time, even when Phantom Co. is closed. Needs their Telegram account to be connected. Give the whole list in one call (up to 100). Right now it is ") + now + " on this PC (the times you give are this PC's local time)."},
         {"inputSchema", schema({{"reminders", {{"type", "array"}, {"description", "The reminders."},
                                                {"items", {{"type", "object"}, {"properties", {{"at", {{"type", "string"}, {"description", "Local date and time as YYYY-MM-DD HH:MM (24 hours), e.g. 2026-11-16 16:00."}}},
                                                                                              {"text", {{"type", "string"}, {"description", "The message, at most 500 characters. Say what to do and for which club or project."}}}}},
                                                           {"required", json::array({"at", "text"})}}}}}}, {"reminders"})}},
        {{"name", "list_reminders"}, {"description", "List the reminders of the person you work for that have not been sent yet (id, time, text)."},
         {"inputSchema", schema(json::object(), json::array())}},
        {{"name", "cancel_reminders"}, {"description", "Cancel reminders that have not been sent yet, by the ids list_reminders shows."},
         {"inputSchema", schema({{"ids", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Reminder ids."}}}}, {"ids"})}},
    });
}

static json reminders_call(const std::string& tool, const json& a, const json& session) {
    const json& sec = session["secrets"];
    const std::string base = worker_base(sec);
    if (base.empty() || sec.value("worker_key", "").empty()) return text_result("The reminder Worker is not set up yet: the owner adds its address and key in Settings > MCP > Reminders.", true);
    if (base.rfind("https://", 0) != 0 && base.rfind("http://", 0) != 0) return text_result("The Worker address must start with https://.", true);
    std::string chat;
    if (session.contains("telegram_ids") && session["telegram_ids"].is_array() && !session["telegram_ids"].empty()) chat = session["telegram_ids"][0].get<std::string>();
    if (chat.empty()) return text_result("The person you work for has not connected Telegram yet. They connect it in Settings > Personal > Telegram (a link code for the bot); reminders can only go to a connected account.", true);

    if (tool == "schedule_reminders") {
        if (!a.contains("reminders") || !a["reminders"].is_array() || a["reminders"].empty()) return text_result("Give a list of reminders, each with `at` and `text`.", true);
        if (a["reminders"].size() > 100) return text_result("At most 100 reminders in one call: split the list.", true);
        const long long now = (long long)std::time(nullptr);
        json items = json::array();
        std::string problems;
        int n = 0;
        for (auto& r : a["reminders"]) {
            ++n;
            const std::string at_s = r.is_object() ? sarg(r, "at") : "", text = r.is_object() ? sarg(r, "text") : "";
            long long at = 0;
            if (text.empty()) { problems += "#" + std::to_string(n) + ": no text. "; continue; }
            if (!parse_local(at_s, at)) { problems += "#" + std::to_string(n) + ": time \"" + at_s + "\" is not YYYY-MM-DD HH:MM. "; continue; }
            if (at < now - 60) { problems += "#" + std::to_string(n) + ": " + at_s + " is already past. "; continue; }
            items.push_back({{"at", at}, {"text", truncate_utf8(text, 500)}});
        }
        if (!problems.empty()) return text_result("Nothing was scheduled. Fix and send the whole list again: " + problems, true);  // all or nothing, so a retry never doubles
        auto r = request("POST", base + "/reminders", worker_headers(sec), json{{"chat_id", chat}, {"reminders", items}}.dump(), "application/json");
        json j = parse(r);
        if (!r || r->status != 200 || !j.is_object()) return text_result(worker_error(r, j), true);
        return text_result("Scheduled " + std::to_string(items.size()) + " reminder(s). They will arrive in the person's Telegram at the given times.");
    }
    if (tool == "list_reminders") {
        auto r = request("GET", base + "/reminders?chat=" + url_encode(chat), worker_headers(sec), "", "");
        json j = parse(r);
        if (!r || r->status != 200 || !j.is_object() || !j.contains("reminders")) return text_result(worker_error(r, j), true);
        std::string out;
        for (auto& x : j["reminders"]) out += "#" + std::to_string(x.value("id", 0LL)) + " · " + local_text(x.value("at", 0LL)) + " · " + x.value("text", "") + "\n";
        return text_result(out.empty() ? "No reminders waiting." : out);
    }
    if (tool == "cancel_reminders") {
        if (!a.contains("ids") || !a["ids"].is_array() || a["ids"].empty()) return text_result("Give the ids to cancel.", true);
        json ids = json::array();
        for (auto& i : a["ids"]) if (i.is_number_integer()) ids.push_back(i);
        auto r = request("POST", base + "/reminders/cancel", worker_headers(sec), json{{"chat_id", chat}, {"ids", ids}}.dump(), "application/json");
        json j = parse(r);
        if (!r || r->status != 200 || !j.is_object()) return text_result(worker_error(r, j), true);
        return text_result("Cancelled " + std::to_string(j.value("removed", 0)) + " reminder(s).");
    }
    return text_result("Unknown tool: " + tool, true);
}

static json reminders_check(const json& sec) {
    const std::string base = worker_base(sec);
    if (base.empty() || sec.value("worker_key", "").empty()) return {{"ok", false}, {"text", "No Worker address or key yet."}};
    if (base.rfind("https://", 0) != 0 && base.rfind("http://", 0) != 0) return {{"ok", false}, {"text", "The address must start with https://"}};
    auto r = request("GET", base + "/health", worker_headers(sec), "", "", 20000, 20000);
    json j = parse(r);
    if (!r || r->status != 200 || !j.is_object()) return {{"ok", false}, {"text", worker_error(r, j)}};
    if (!j.value("telegram", true)) return {{"ok", false}, {"text", "The Worker has no BOT_TOKEN secret yet."}};
    return {{"ok", true}, {"text", std::to_string(j.value("waiting", 0)) + " reminder(s) waiting"}};
}

// ------------------------------------------------------------------- public
json tools(const std::string& builtin) {
    if (builtin == "reminders") return reminders_tools();
    if (builtin == "telegram") return telegram_tools();
    if (builtin == "youtube") return youtube_tools();
    if (builtin == "reddit") return reddit_tools();
    return json::array();
}

json call(const std::string& builtin, const std::string& tool, const json& args, const json& session) {
    const json a = args.is_object() ? args : json::object();
    try {
        if (builtin == "telegram") return telegram_call(tool, a, session);
        if (builtin == "youtube") return youtube_call(tool, a, session);
        if (builtin == "reddit") return reddit_call(tool, a, session);
        if (builtin == "reminders") return reminders_call(tool, a, session);
    } catch (const std::exception& e) {
        return text_result(std::string("Error: ") + e.what(), true);
    }
    return text_result("Unknown server.", true);
}

json check(const std::string& builtin, const json& secrets, const json& options) {
    std::string error;
    if (builtin == "telegram") {
        if (secrets.value("bot_token", "").empty()) return {{"ok", false}, {"text", "No bot token yet."}};
        json me = tg_api(secrets.value("bot_token", ""), "getMe", "{}", "application/json", error);
        if (me.is_null()) return {{"ok", false}, {"text", error}};
        return {{"ok", true}, {"text", "@" + me.value("username", "")}};
    }
    if (builtin == "youtube") {
        if (secrets.value("client_id", "").empty() || secrets.value("client_secret", "").empty()) return {{"ok", false}, {"text", "No OAuth client yet."}};
        std::string token = google_access(secrets, error);
        if (token.empty()) return {{"ok", false}, {"text", error}};
        json ch = yt_get(token, "channels?part=snippet&mine=true", error);
        if (ch.is_null()) return {{"ok", false}, {"text", error}};
        if (!ch.contains("items") || ch["items"].empty()) return {{"ok", false}, {"text", "This Google account has no YouTube channel yet."}};
        return {{"ok", true}, {"text", ch["items"][0]["snippet"].value("title", "")}};
    }
    if (builtin == "reddit") return reddit_check(secrets);
    if (builtin == "reminders") return reminders_check(secrets);
    return {{"ok", false}, {"text", "Unknown server."}};
}

}  // namespace pc2::builtin
