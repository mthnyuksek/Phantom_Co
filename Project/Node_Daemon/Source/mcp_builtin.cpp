#include "mcp_builtin.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <chrono>
#include <ctime>
#include <memory>

#include "http_client.hpp"

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
    DWORD n = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string b(n, '\0');
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b.data(), &n);
    b.resize(n);
    std::string out;
    for (char c : b) { if (c == '+') out += '-'; else if (c == '/') out += '_'; else if (c != '=' && c != '\0') out += c; }
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

// ------------------------------------------------------------------- public
json tools(const std::string& builtin) {
    if (builtin == "telegram") return telegram_tools();
    if (builtin == "youtube") return youtube_tools();
    return json::array();
}

json call(const std::string& builtin, const std::string& tool, const json& args, const json& session) {
    const json a = args.is_object() ? args : json::object();
    try {
        if (builtin == "telegram") return telegram_call(tool, a, session);
        if (builtin == "youtube") return youtube_call(tool, a, session);
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
    return {{"ok", false}, {"text", "Unknown server."}};
}

}  // namespace pc2::builtin
