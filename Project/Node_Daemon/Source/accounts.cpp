#include "accounts.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "http_client.hpp"
#include "process.hpp"
#include "providers.hpp"

namespace pc2 {

Accounts::Accounts(const fs::path& data_dir) : dir_(data_dir) {
    json j = read_json(dir_ / L"accounts.json");
    if (j.contains("accounts") && j["accounts"].is_array())
        for (auto& a : j["accounts"])
            if (a.is_object() && valid_id(a.value("id", "")) && (a.value("provider", "") == "claude" || a.value("provider", "") == "codex" || a.value("provider", "") == "gemini" || a.value("provider", "") == "opencode")) {
                json item = {{"id", a["id"]}, {"provider", a["provider"]}, {"kind", a.value("kind", "") == "api_key" ? "api_key" : "subscription"},
                             {"name", a.value("name", "")}, {"created", a.value("created", "")}, {"key_hint", a.value("key_hint", "")}};
                if (a.value("provider", "") == "opencode") item["service"] = a.value("service", "");  // OpenCode: the model service of the key
                items_.push_back(item);
            }
}

// ---- API keys: encrypted with DPAPI for this Windows user (another user or PC cannot read them), base64 in secrets.json
static const char kEntropy[] = "Phantom Co. account key";

static std::string b64(const std::string& raw) {
    DWORD n = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string out(n, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(raw.data()), (DWORD)raw.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n)) return "";
    out.resize(n);
    return out;
}
static std::string unb64(const std::string& text) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(text.c_str(), (DWORD)text.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return "";
    std::string out(n, '\0');
    if (!CryptStringToBinaryA(text.c_str(), (DWORD)text.size(), CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &n, nullptr, nullptr)) return "";
    out.resize(n);
    return out;
}

bool Accounts::valid_key(const std::string& key) {
    if (key.size() < 20 || key.size() > 400) return false;
    for (unsigned char c : key)
        if (c < 0x21 || c > 0x7e || c == '"' || c == '\'' || c == '`' || c == '\\') return false;
    return true;
}

void Accounts::store_key(const std::string& id, const std::string& key) {  // caller holds mu_
    DATA_BLOB in{(DWORD)key.size(), reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))}, ent{sizeof(kEntropy), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))}, out{};
    if (!CryptProtectData(&in, L"Phantom Co. account key", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    std::string enc = b64(std::string(reinterpret_cast<char*>(out.pbData), out.cbData));
    LocalFree(out.pbData);
    json s = read_json(dir_ / L"secrets.json");
    if (!s.contains("keys") || !s["keys"].is_object()) s["keys"] = json::object();
    s["keys"][id] = enc;
    write_json(dir_ / L"secrets.json", s);
}

std::string Accounts::key_of(const std::string& id) {
    json s = read_json(dir_ / L"secrets.json");
    if (!s.contains("keys") || !s["keys"].is_object() || !s["keys"].contains(id) || !s["keys"][id].is_string()) return "";
    std::string raw = unb64(s["keys"][id].get<std::string>());
    DATA_BLOB in{(DWORD)raw.size(), reinterpret_cast<BYTE*>(raw.data())}, ent{sizeof(kEntropy), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))}, out{};
    if (raw.empty() || !CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string key(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key;
}

bool Accounts::set_key(const std::string& id, const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& a : items_)
        if (a["id"] == id && a["kind"] == "api_key") {
            store_key(id, key);
            a["key_hint"] = "…" + key.substr(key.size() - 4);
            save();
            return true;
        }
    return false;
}

void Accounts::save() { write_json(dir_ / L"accounts.json", json{{"accounts", items_}}); }

json Accounts::list() {
    std::lock_guard<std::mutex> l(mu_);
    return items_;
}

std::optional<json> Accounts::get(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& a : items_) if (a["id"] == id) return a;
    return std::nullopt;
}

json Accounts::add(const std::string& provider, const std::string& name, const std::string& kind, const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    std::string id;
    for (int i = 0; i < 100 && (id.empty() || std::any_of(items_.begin(), items_.end(), [&](const json& a) { return a["id"] == id; })); ++i) {
        std::string r = new_id().substr(18);
        for (auto& c : r) c = (char)tolower((unsigned char)c);
        id = "acct_" + r;
    }
    const bool api = kind == "api_key";
    json a = {{"id", id}, {"provider", provider}, {"kind", api ? "api_key" : "subscription"}, {"name", name}, {"created", now_iso()},
              {"key_hint", api && key.size() >= 4 ? "…" + key.substr(key.size() - 4) : ""}};
    if (provider == "opencode") { auto sv = opencode_service_for_key(key); a["service"] = sv ? sv->id : ""; }
    std::error_code ec;
    fs::create_directories(home(id), ec);
    if (api) store_key(id, key);
    items_.push_back(a);
    save();
    return a;
}

bool Accounts::rename(const std::string& id, const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& a : items_)
        if (a["id"] == id) { a["name"] = name; save(); return true; }
    return false;
}

bool Accounts::remove(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = std::find_if(items_.begin(), items_.end(), [&](const json& a) { return a["id"] == id; });
    if (it == items_.end()) return false;
    items_.erase(it);
    save();
    json s = read_json(dir_ / L"secrets.json");
    if (s.contains("keys") && s["keys"].is_object() && s["keys"].contains(id)) { s["keys"].erase(id); write_json(dir_ / L"secrets.json", s); }
    std::error_code ec;
    fs::remove_all(home(id), ec);  // the sign-in goes with it (a file still in use stays until the next try; harmless)
    return true;
}

fs::path Accounts::home(const std::string& id) const { return dir_ / L"Accounts" / P(id); }

std::vector<std::pair<std::string, std::string>> Accounts::env(const std::string& provider, const std::string& id) {
    if (id.empty()) return {};
    const std::string h = U8(home(id));
    auto a = get(id);
    if (a && (*a)["kind"] == "api_key") {
        // The key goes only into this run's environment (never on a command line or into a file of the run).
        std::string key = key_of(id);
        if (provider == "claude") return {{"CLAUDE_CONFIG_DIR", h}, {"ANTHROPIC_API_KEY", key}, {"ANTHROPIC_AUTH_TOKEN", ""}};
        if (provider == "codex") return {{"CODEX_HOME", h}, {"CODEX_API_KEY", key}, {"OPENAI_API_KEY", key}};
        if (provider == "gemini") return {{"GEMINI_CLI_HOME", h}, {"GEMINI_API_KEY", key}, {"GOOGLE_API_KEY", ""}, {"GOOGLE_GENAI_USE_VERTEXAI", ""}, {"GOOGLE_GENAI_USE_GCA", ""}};
        if (provider == "opencode") {
            // XDG_DATA_HOME = the account folder: OpenCode keeps its sessions (and any sign-in of its own) there.
            std::vector<std::pair<std::string, std::string>> e = {{"XDG_DATA_HOME", h}};
            for (auto& s : opencode_services()) e.push_back({s.env, s.id == a->value("service", "") ? key : ""});
            return e;
        }
        return {};
    }
    // A key in the PC's environment would win over the account's subscription sign-in: leave it out for account runs.
    if (provider == "claude") return {{"CLAUDE_CONFIG_DIR", h}, {"ANTHROPIC_API_KEY", ""}, {"ANTHROPIC_AUTH_TOKEN", ""}};
    if (provider == "codex") return {{"CODEX_HOME", h}, {"OPENAI_API_KEY", ""}, {"CODEX_API_KEY", ""}};
    // Gemini keeps everything in <GEMINI_CLI_HOME>\.gemini (settings.json with the chosen sign-in, oauth_creds.json)
    if (provider == "gemini") return {{"GEMINI_CLI_HOME", h}, {"GEMINI_API_KEY", ""}, {"GOOGLE_API_KEY", ""}, {"GOOGLE_GENAI_USE_VERTEXAI", ""}, {"GOOGLE_GENAI_USE_GCA", ""}};
    if (provider == "opencode") return {{"XDG_DATA_HOME", h}};
    return {};
}

// Runs a short CLI command and collects its output; killed after `timeout`.
static int run_capture(ProcessSpec spec, std::chrono::seconds timeout, std::string& out) {
    Process p;
    std::string err;
    if (!p.start(spec, err)) { out = err; return -1; }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, timeout, [&] { return done; })) p.kill();
    });
    p.read_lines([&](const std::string& line) { if (out.size() < 65536) out += line + "\n"; });
    int code = p.wait();
    { std::lock_guard<std::mutex> lk(m); done = true; }
    cv.notify_all();
    watchdog.join();
    return code;
}

static std::string b64url_decode(std::string s) {
    for (auto& c : s) c = c == '-' ? '+' : c == '_' ? '/' : c;
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char* f = c ? strchr(tbl, c) : nullptr;
        if (!f) break;
        val = (val << 6) + int(f - tbl);
        bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

static std::string env_var(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? Narrow(std::wstring(buf, n)) : "";
}

json Accounts::status(const std::string& provider, const std::string& id) {
    json st = {{"signed_in", false}, {"email", ""}, {"plan", ""}, {"error", ""}};
    auto acc = id.empty() ? std::nullopt : get(id);
    if (acc && (*acc)["kind"] == "api_key") {
        // The provider's model list costs nothing and answers 401 for a wrong key.
        st["plan"] = "api key";
        std::string key = key_of(id);
        if (key.empty()) { st["error"] = "key_unreadable"; return st; }
        if (provider == "opencode") {
            // NVIDIA's model list is public (200 for any key): the smallest chat request (1 token) shows whether the key works.
            const OpenCodeService* sv = opencode_service((*acc).value("service", ""));
            if (!sv) { st["error"] = "unknown"; return st; }
            st["plan"] = std::string(sv->label) + " api key";
            HttpClient http("https://integrate.api.nvidia.com", 8000, 20000);
            std::string model = sv->model;
            model = model.substr(model.find('/') + 1);  // "nvidia/nvidia/x" -> "nvidia/x" (OpenCode's service prefix off)
            auto r = http.Post("/v1/chat/completions", {{"Authorization", "Bearer " + key}},
                               json{{"model", model}, {"max_tokens", 1}, {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}}.dump(), "application/json");
            if (!r) st["error"] = "unreachable";
            else if (r->status == 200 || r->status == 429) st["signed_in"] = true;  // 429: the key is fine, only busy
            else if (r->status == 401 || r->status == 403) st["error"] = "key_rejected";
            else st["error"] = "unreachable";
            return st;
        }
        const bool claude = provider == "claude", gemini = provider == "gemini";
        HttpClient http(claude ? "https://api.anthropic.com" : gemini ? "https://generativelanguage.googleapis.com" : "https://api.openai.com", 8000, 12000);
        Headers h = claude ? Headers{{"x-api-key", key}, {"anthropic-version", "2023-06-01"}}
                  : gemini ? Headers{{"x-goog-api-key", key}}  // a header, so the key never sits in a URL
                           : Headers{{"Authorization", "Bearer " + key}};
        auto r = http.Get(gemini ? "/v1beta/models" : "/v1/models", h);
        if (!r) st["error"] = "unreachable";
        else if (r->status == 200) st["signed_in"] = true;
        else if (r->status == 401 || r->status == 403 || (gemini && r->status == 400)) st["error"] = "key_rejected";  // Google: 400 API_KEY_INVALID
        else st["error"] = "unreachable";
        return st;
    }
    if (provider == "opencode") {
        // Auto = a model service key in the PC's environment, or OpenCode's own sign-ins (<data>\opencode\auth.json, never read out).
        if (provider_exe("opencode").empty()) { st["error"] = "not_installed"; return st; }
        for (auto& s : opencode_services())
            if (!env_var(Widen(s.env).c_str()).empty()) { st["signed_in"] = true; st["plan"] = std::string(s.label) + " api key"; return st; }
        std::string xdg = env_var(L"XDG_DATA_HOME");
        fs::path auth = (xdg.empty() ? P(env_var(L"USERPROFILE")) / L".local" / L"share" : P(xdg)) / L"opencode" / L"auth.json";
        json a = read_json(auth);
        if (a.is_object() && !a.empty()) { st["signed_in"] = true; st["plan"] = "opencode sign-in"; }
        return st;
    }
    if (provider == "gemini") {
        // Gemini CLI has no status command: read what its own sign-in left in <home>\.gemini (never sent anywhere).
        if (provider_exe("gemini").empty()) { st["error"] = "not_installed"; return st; }
        std::string h = id.empty() ? env_var(L"GEMINI_CLI_HOME") : U8(home(id));
        fs::path g = (h.empty() ? P(env_var(L"USERPROFILE")) : P(h)) / L".gemini";
        json settings = read_json(g / L"settings.json");
        std::string type;
        if (settings.contains("security") && settings["security"].is_object() && settings["security"].contains("auth") &&
            settings["security"]["auth"].is_object())
            type = settings["security"]["auth"].value("selectedType", "");
        std::error_code ec;
        if (id.empty() && !env_var(L"GEMINI_API_KEY").empty()) { st["signed_in"] = true; st["plan"] = "api key"; }  // Auto with a key in the PC's environment
        else if (type == "oauth-personal" || type == "login-with-google") {
            // Since late 2026 Google refuses the free Google sign-in for Gemini CLI (IneligibleTierError, "migrate to Antigravity").
            st["error"] = "google_unsupported";
            json acc = read_json(g / L"google_accounts.json");
            if (acc.contains("active") && acc["active"].is_string()) st["email"] = acc["active"];
        } else if (!type.empty() && type != "oauth-personal") {
            st["signed_in"] = true;  // Vertex AI, Code Assist and other company sign-ins: trust the CLI's own setting
            st["plan"] = type;
        }
        return st;
    }
    ProcessSpec ps;
    ps.exe = provider_exe(provider);
    if (ps.exe.empty()) { st["error"] = "not_installed"; return st; }
    ps.cwd = U8(id.empty() ? dir_ : home(id));
    std::error_code ec;
    fs::create_directories(P(ps.cwd), ec);
    ps.env = env(provider, id);
    std::string out;
    if (provider == "claude") {
        ps.args = {"auth", "status", "--json"};
        run_capture(ps, std::chrono::seconds(20), out);
        size_t a = out.find('{'), b = out.rfind('}');
        json j = a != std::string::npos && b > a ? json::parse(out.substr(a, b - a + 1), nullptr, false) : json();
        if (!j.is_object()) { st["error"] = "unknown"; return st; }
        st["signed_in"] = j.value("loggedIn", false);
        if (j.contains("email") && j["email"].is_string()) st["email"] = j["email"];
        if (j.contains("subscriptionType") && j["subscriptionType"].is_string()) st["plan"] = j["subscriptionType"];
        else if (j.value("authMethod", "") != "claude.ai" && j.value("authMethod", "") != "none") st["plan"] = j.value("authMethod", "");
        return st;
    }
    if (provider == "codex") {
        ps.args = {"login", "status"};
        int code = run_capture(ps, std::chrono::seconds(20), out);
        st["signed_in"] = code == 0;
        // e-mail and plan: from the id token the CLI keeps in <CODEX_HOME>\auth.json (read only, never sent anywhere)
        std::string h = id.empty() ? env_var(L"CODEX_HOME") : U8(home(id));
        fs::path home_dir = h.empty() ? P(env_var(L"USERPROFILE")) / L".codex" : P(h);
        json auth = read_json(home_dir / L"auth.json");
        if (auth.value("auth_mode", "") == "apikey") st["plan"] = "api key";
        if (auth.contains("tokens") && auth["tokens"].is_object() && auth["tokens"].contains("id_token") && auth["tokens"]["id_token"].is_string()) {
            std::string t = auth["tokens"]["id_token"];
            size_t a = t.find('.'), b = t.find('.', a + 1);
            json claims = a != std::string::npos && b != std::string::npos ? json::parse(b64url_decode(t.substr(a + 1, b - a - 1)), nullptr, false) : json();
            if (claims.is_object()) {
                if (claims.contains("email") && claims["email"].is_string()) st["email"] = claims["email"];
                const char* k = "https://api.openai.com/auth";
                if (claims.contains(k) && claims[k].is_object() && claims[k].contains("chatgpt_plan_type") && claims[k]["chatgpt_plan_type"].is_string())
                    st["plan"] = claims[k]["chatgpt_plan_type"];
            }
        }
        return st;
    }
    st["error"] = "unknown";
    return st;
}

bool Accounts::open_login(const std::string& provider, const std::string& id, std::string& error) {
    if (auto a = id.empty() ? std::nullopt : get(id); a && (*a)["kind"] == "api_key") { error = "This account uses an API key: there is nothing to sign in."; return false; }
    if (provider == "gemini") { error = kGeminiNoGoogle; return false; }
    if (provider == "opencode") { error = kOpenCodeKeyOnly; return false; }
    std::string exe = provider_exe(provider);
    if (exe.empty()) { error = provider == "claude" ? "Claude Code CLI was not found on this PC." : provider == "codex" ? "Codex CLI was not found on this PC." : "Gemini CLI was not found on this PC."; return false; }
    fs::path cwd = id.empty() ? dir_ : home(id);
    std::error_code ec;
    fs::create_directories(cwd, ec);
    // Nothing typed by people goes into this command line (only the CLI path we found ourselves).
    std::wstring cli = L"\"" + Widen(exe) + L"\" " + (provider == "claude" ? L"auth login" : L"login");
    std::wstring cmd = L"cmd.exe /d /s /c \"title Phantom Co. sign-in & " + cli +
                       L" & echo. & echo Done: you can close this window. / Bitti: bu pencereyi kapatabilirsin. & pause >nul\"";
    std::wstring envb = environment_block(env(provider, id));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        envb.empty() ? nullptr : envb.data(), cwd.c_str(), &si, &pi)) {
        error = "The sign-in window could not be opened (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace pc2
