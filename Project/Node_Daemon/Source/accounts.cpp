#include "accounts.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "process.hpp"
#include "providers.hpp"

namespace pc2 {

Accounts::Accounts(const fs::path& data_dir) : dir_(data_dir) {
    json j = read_json(dir_ / L"accounts.json");
    if (j.contains("accounts") && j["accounts"].is_array())
        for (auto& a : j["accounts"])
            if (a.is_object() && valid_id(a.value("id", "")) && (a.value("provider", "") == "claude" || a.value("provider", "") == "codex"))
                items_.push_back({{"id", a["id"]}, {"provider", a["provider"]}, {"name", a.value("name", "")}, {"created", a.value("created", "")}});
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

json Accounts::add(const std::string& provider, const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    std::string id;
    for (int i = 0; i < 100 && (id.empty() || std::any_of(items_.begin(), items_.end(), [&](const json& a) { return a["id"] == id; })); ++i) {
        std::string r = new_id().substr(18);
        for (auto& c : r) c = (char)tolower((unsigned char)c);
        id = "acct_" + r;
    }
    json a = {{"id", id}, {"provider", provider}, {"name", name}, {"created", now_iso()}};
    std::error_code ec;
    fs::create_directories(home(id), ec);
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
    std::error_code ec;
    fs::remove_all(home(id), ec);  // the sign-in goes with it (a file still in use stays until the next try; harmless)
    return true;
}

fs::path Accounts::home(const std::string& id) const { return dir_ / L"Accounts" / P(id); }

std::vector<std::pair<std::string, std::string>> Accounts::env(const std::string& provider, const std::string& id) const {
    if (id.empty()) return {};
    const std::string h = U8(home(id));
    // A key in the PC's environment would win over the account's subscription sign-in: leave it out for account runs.
    if (provider == "claude") return {{"CLAUDE_CONFIG_DIR", h}, {"ANTHROPIC_API_KEY", ""}, {"ANTHROPIC_AUTH_TOKEN", ""}};
    if (provider == "codex") return {{"CODEX_HOME", h}, {"OPENAI_API_KEY", ""}, {"CODEX_API_KEY", ""}};
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
    std::string exe = provider_exe(provider);
    if (exe.empty()) { error = provider == "claude" ? "Claude Code CLI was not found on this PC." : "Codex CLI was not found on this PC."; return false; }
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
