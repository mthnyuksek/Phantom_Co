#include "mcp.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <chrono>
#include <regex>

namespace pc2 {

// ------------------------------------------------------------------ secrets
static const char kEntropy[] = "Phantom Co. MCP secret";

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

std::string seal_secret(const std::string& plain, const wchar_t* what) {
    DATA_BLOB in{(DWORD)plain.size(), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))},
        ent{sizeof(kEntropy), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))}, out{};
    if (!CryptProtectData(&in, what, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string enc = b64(std::string(reinterpret_cast<char*>(out.pbData), out.cbData));
    LocalFree(out.pbData);
    return enc;
}

std::string open_secret(const std::string& sealed) {
    std::string raw = unb64(sealed);
    DATA_BLOB in{(DWORD)raw.size(), reinterpret_cast<BYTE*>(raw.data())}, ent{sizeof(kEntropy), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))}, out{};
    if (raw.empty() || !CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

static std::string random_hex(size_t n) {
    std::string b(n, '\0'), out;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(b.data()), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : b) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}

// ------------------------------------------------------------------ the list
static bool valid_name(const std::string& s) { return std::regex_match(s, std::regex("[a-z][a-z0-9_]{0,23}")); }
static bool valid_env(const std::string& s) { return std::regex_match(s, std::regex("[A-Z_][A-Z0-9_]{0,63}")); }
static bool valid_tool(const std::string& s) { return std::regex_match(s, std::regex("[A-Za-z0-9_.-]{1,64}")); }
static bool one_line(const std::string& s) { return s.find_first_of(std::string("\r\n\0", 3)) == std::string::npos; }

// Secret names each kind of server keeps. A command server keeps the environment variables it names.
static std::set<std::string> builtin_secrets(const std::string& builtin) {
    if (builtin == "telegram") return {"bot_token"};
    if (builtin == "youtube") return {"client_id", "client_secret", "refresh_token"};
    return {};
}

Mcp::Mcp(const fs::path& data_dir, Store& store) : dir_(data_dir), store_(store) { load(); }

void Mcp::load() {
    json j = read_json(dir_ / L"mcp.json");
    items_ = json::array();
    if (j.contains("servers") && j["servers"].is_array())
        for (auto& s : j["servers"])
            if (s.is_object() && valid_name(s.value("name", ""))) items_.push_back(s);
}

void Mcp::save_list() { write_json(dir_ / L"mcp.json", {{"servers", items_}}); }

json Mcp::catalog() {
    return json::array({
        {{"name", "browser"}, {"label", "Browser"}, {"kind", "command"}, {"command", "npx"},
         {"args", {"-y", "@playwright/mcp@0.0.83", "--headless", "--isolated"}}, {"approve", false}, {"skill", "browser"}},
        {{"name", "youtube"}, {"label", "YouTube"}, {"kind", "builtin"}, {"builtin", "youtube"}, {"approve", true},
         {"approve_tools", {"upload_video", "set_thumbnail"}}, {"skill", "youtube"}},
        {{"name", "telegram"}, {"label", "Telegram"}, {"kind", "builtin"}, {"builtin", "telegram"}, {"approve", true},
         {"approve_tools", {"send_message", "send_file"}}, {"skill", "telegram"}},
    });
}

json Mcp::list() {
    std::lock_guard<std::mutex> l(mu_);
    json s = read_json(dir_ / L"secrets.json");
    json out = json::array();
    for (auto x : items_) {
        json set = json::array();
        const std::string n = x["name"];
        if (s.contains("mcp") && s["mcp"].contains(n) && s["mcp"][n].is_object())
            for (auto& [k, v] : s["mcp"][n].items()) if (v.is_string() && !v.get<std::string>().empty()) set.push_back(k);
        x["secrets_set"] = set;
        out.push_back(x);
    }
    return out;
}

std::optional<json> Mcp::get(const std::string& name) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& x : items_) if (x["name"] == name) return x;
    return std::nullopt;
}

std::string Mcp::label_of(const std::string& name) {  // caller holds mu_
    for (auto& x : items_) if (x["name"] == name) return x.value("label", name);
    return name;
}

std::string Mcp::secret(const std::string& name, const std::string& key) {
    json s = read_json(dir_ / L"secrets.json");
    if (!s.contains("mcp") || !s["mcp"].contains(name) || !s["mcp"][name].contains(key) || !s["mcp"][name][key].is_string()) return "";
    return open_secret(s["mcp"][name][key].get<std::string>());
}

void Mcp::set_secret(const std::string& name, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> l(mu_);
    json s = read_json(dir_ / L"secrets.json");
    if (!s.is_object()) s = json::object();
    if (!s.contains("mcp") || !s["mcp"].is_object()) s["mcp"] = json::object();
    if (!s["mcp"].contains(name) || !s["mcp"][name].is_object()) s["mcp"][name] = json::object();
    if (value.empty()) s["mcp"][name].erase(key);
    else s["mcp"][name][key] = seal_secret(value, L"Phantom Co. MCP secret");
    write_json(dir_ / L"secrets.json", s);
}

// Copies a server's own folder into <data>\Mcp\<name> (outside the Vault: ghosts cannot change it after the owner checked it).
static bool copy_folder(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) { error = "That folder does not exist."; return false; }
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), from, ec);
        if (it->path().filename() == L".git") { it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) { fs::create_directories(to / rel, ec); continue; }
        if (!it->is_regular_file(ec)) continue;
        total += it->file_size(ec);
        if (total > 300ull * 1024 * 1024) { error = "The folder is larger than 300 MB."; fs::remove_all(to, ec); return false; }
        fs::create_directories((to / rel).parent_path(), ec);
        fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "Could not copy " + U8(rel) + "."; return false; }
    }
    return true;
}

bool Mcp::save(const json& in, const std::map<std::string, std::string>& secrets, bool is_new, std::string& error) {
    const std::string name = in.value("name", "");
    if (!valid_name(name)) { error = "The name must start with a letter: a-z, 0-9 and _ only, at most 24 characters."; return false; }
    json base;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& x : items_) if (x["name"] == name) base = x;
    }
    if (is_new && !base.is_null()) { error = "There is already a server with this name."; return false; }
    if (!is_new && base.is_null()) { error = "Server not found."; return false; }
    if (base.is_null()) base = {{"name", name}, {"created", now_iso()}};

    json out = base;
    auto s = [&](const char* k, size_t max, const std::string& def) {
        std::string v = in.contains(k) && in[k].is_string() ? in[k].get<std::string>() : base.value(k, def);
        return truncate_utf8(v, max);
    };
    out["label"] = s("label", 40, name);
    if (out["label"].get<std::string>().empty()) out["label"] = name;
    out["kind"] = s("kind", 16, "command");
    if (out["kind"] != "command" && out["kind"] != "builtin") { error = "Unknown kind of server."; return false; }
    if (out["kind"] == "builtin") {
        out["builtin"] = s("builtin", 16, "");
        if (out["builtin"] != "telegram" && out["builtin"] != "youtube") { error = "Unknown built-in server."; return false; }
        out.erase("command"); out.erase("args"); out.erase("folder"); out.erase("env_names");
        json opt = json::object();
        if (out["builtin"] == "telegram") {
            const json& src = in.contains("options") && in["options"].is_object() ? in["options"] : base.value("options", json::object());
            std::string chat = src.value("chat_id", "");
            if (!std::regex_match(chat, std::regex("(-?[0-9]{1,20}|@[A-Za-z0-9_]{4,40})?"))) { error = "The chat id is a number (or @channelname)."; return false; }
            opt["chat_id"] = chat;
        }
        out["options"] = opt;
    } else {
        out.erase("builtin"); out.erase("options");
        out["command"] = s("command", 260, "");
        const std::string cmd = out["command"];
        if (cmd.empty() || !one_line(cmd) || cmd.find('"') != std::string::npos) { error = "Give the command that starts the server, e.g. npx, python or a full path."; return false; }
        json args = json::array();
        for (auto& a : in.contains("args") && in["args"].is_array() ? in["args"] : base.value("args", json::array())) {
            if (!a.is_string() || a.get<std::string>().size() > 1000 || !one_line(a.get<std::string>())) { error = "An argument is too long or has a line break."; return false; }
            args.push_back(a);
        }
        if (args.size() > 40) { error = "Too many arguments."; return false; }
        out["args"] = args;
        json envs = json::array();
        for (auto& e : in.contains("env_names") && in["env_names"].is_array() ? in["env_names"] : base.value("env_names", json::array())) {
            if (!e.is_string() || !valid_env(e.get<std::string>())) { error = "Secret names look like API_KEY (A-Z, 0-9 and _)."; return false; }
            envs.push_back(e);
        }
        if (envs.size() > 20) { error = "Too many secrets."; return false; }
        out["env_names"] = envs;
        std::string folder = s("folder", 400, "");
        if (!one_line(folder)) { error = "Invalid folder."; return false; }
        out["folder"] = folder;
        if (!folder.empty() && (is_new || folder != base.value("folder", ""))) {
            if (!P(folder).is_absolute()) { error = "Give the full path of the server's folder."; return false; }
            if (!copy_folder(P(folder), dir_ / L"Mcp" / P(name), error)) return false;
        }
        if (folder.empty()) { std::error_code ec; fs::remove_all(dir_ / L"Mcp" / P(name), ec); }
    }
    out["approve"] = in.contains("approve") && in["approve"].is_boolean() ? in["approve"].get<bool>() : base.value("approve", true);
    json tools = json::array();
    for (auto& t : in.contains("approve_tools") && in["approve_tools"].is_array() ? in["approve_tools"] : base.value("approve_tools", json::array()))
        if (t.is_string() && valid_tool(t.get<std::string>())) tools.push_back(t);
    out["approve_tools"] = tools;
    out["enabled"] = in.contains("enabled") && in["enabled"].is_boolean() ? in["enabled"].get<bool>() : base.value("enabled", true);

    // secrets: only the names this server keeps
    std::set<std::string> allowed;
    if (out["kind"] == "builtin") allowed = builtin_secrets(out["builtin"]);
    else for (auto& e : out["env_names"]) allowed.insert(e.get<std::string>());
    for (auto& [k, v] : secrets) {
        if (!allowed.count(k) || k == "refresh_token") { error = "Unknown secret: " + k; return false; }
        if (v.size() > 4000 || !one_line(v)) { error = "A secret value is too long or has a line break."; return false; }
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        bool found = false;
        for (auto& x : items_) if (x["name"] == name) { x = out; found = true; }
        if (!found) items_.push_back(out);
        save_list();
    }
    for (auto& [k, v] : secrets) if (!v.empty()) set_secret(name, k, v);
    {   // forget values of secrets the server no longer has
        json st = read_json(dir_ / L"secrets.json");
        if (st.contains("mcp") && st["mcp"].contains(name) && st["mcp"][name].is_object())
            for (auto& [k, v] : st["mcp"][name].items()) if (!allowed.count(k)) set_secret(name, k, "");
    }
    return true;
}

bool Mcp::remove(const std::string& name) {
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = std::find_if(items_.begin(), items_.end(), [&](const json& x) { return x["name"] == name; });
        if (it == items_.end()) return false;
        items_.erase(it);
        save_list();
        json s = read_json(dir_ / L"secrets.json");
        if (s.contains("mcp") && s["mcp"].contains(name)) { s["mcp"].erase(name); write_json(dir_ / L"secrets.json", s); }
    }
    std::error_code ec;
    fs::remove_all(dir_ / L"Mcp" / P(name), ec);
    return true;
}

bool Mcp::refresh_folder(const std::string& name, std::string& error) {
    auto x = get(name);
    if (!x || x->value("folder", "").empty()) { error = "This server has no folder."; return false; }
    return copy_folder(P(x->value("folder", "")), dir_ / L"Mcp" / P(name), error);
}

// --------------------------------------------------------------------- runs
std::string Mcp::open_run(const McpRun& r) {
    std::lock_guard<std::mutex> l(mu_);
    std::string key = random_hex(24);
    runs_[key] = r;
    return key;
}

void Mcp::close_run(const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    runs_.erase(key);
    for (auto it = approvals_.begin(); it != approvals_.end();) {
        if (it->second.key == key) {
            if (it->second.status == "pending") store_.conv_append(it->second.conv, {{"t", "msg"}, {"from", "system"}, {"text", "The request to use " + it->second.label + " › " + it->second.tool + " ended with the run."}});
            it = approvals_.erase(it);
        } else ++it;
    }
    cv_.notify_all();
}

std::optional<json> Mcp::session(const std::string& key, const std::string& name, std::string& error) {
    McpRun run;
    json x;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto r = runs_.find(key);
        if (r == runs_.end()) { error = "Unknown or finished run."; return std::nullopt; }
        run = r->second;
        if (!run.servers.count(name)) { error = "This run may not use that server."; return std::nullopt; }
        for (auto& i : items_) if (i["name"] == name) x = i;
    }
    if (x.is_null() || !x.value("enabled", true)) { error = "The server is missing or switched off."; return std::nullopt; }
    json out = {{"name", name}, {"label", x.value("label", name)}, {"kind", x["kind"]}, {"workdir", run.workdir},
                {"approve", x.value("approve", true)}, {"approve_tools", x.value("approve_tools", json::array())}};
    if (x["kind"] == "builtin") {
        out["builtin"] = x["builtin"];
        out["options"] = x.value("options", json::object());
        json sec = json::object();
        for (auto& k : builtin_secrets(x["builtin"])) sec[k] = secret(name, k);
        out["secrets"] = sec;
    } else {
        out["command"] = x["command"];
        out["args"] = x["args"];
        out["cwd"] = x.value("folder", "").empty() ? run.workdir : U8(dir_ / L"Mcp" / P(name));
        json env = json::object();
        for (auto& e : x["env_names"]) env[e.get<std::string>()] = secret(name, e.get<std::string>());
        out["env"] = env;
    }
    return out;
}

// ---------------------------------------------------------------- approvals
json Mcp::ask(const std::string& key, const std::string& server, const std::string& tool, const json& args, const std::string& id_in, int wait_s) {
    std::unique_lock<std::mutex> l(mu_);
    auto r = runs_.find(key);
    if (r == runs_.end() || !r->second.servers.count(server)) return {{"id", id_in}, {"status", "cancelled"}};
    std::string id = id_in;
    if (id.empty()) {
        Approval a;
        a.id = new_id();
        a.key = key;
        a.run_id = r->second.run_id;
        a.agent_id = r->second.agent_id;
        a.user_id = r->second.user_id;
        a.conv = r->second.conv;
        a.server = server;
        a.label = label_of(server);
        a.tool = truncate_utf8(tool, 64);
        a.args = truncate_utf8(args.is_null() ? "{}" : args.dump(2), 2000);
        auto ag = store_.agent(a.agent_id);
        const std::string who = ag ? ag->value("name", "A ghost") : "A ghost";
        store_.conv_append(a.conv, {{"t", "msg"}, {"from", "system"}, {"kind", "approval"}, {"approval", a.id}, {"agent", a.agent_id},
                                    {"server", a.label}, {"tool", a.tool}, {"args", a.args}, {"text", who + " asks to use " + a.label + " › " + a.tool}});
        id = a.id;
        approvals_[id] = a;
    }
    cv_.wait_for(l, std::chrono::seconds(wait_s), [&] {
        auto it = approvals_.find(id);
        return it == approvals_.end() || it->second.status != "pending";
    });
    auto it = approvals_.find(id);
    if (it == approvals_.end()) return {{"id", id}, {"status", "cancelled"}};
    return {{"id", id}, {"status", it->second.status}, {"by", it->second.by}};
}

json Mcp::pending(const std::string& conv, const std::string& user_id, bool owner) {
    std::lock_guard<std::mutex> l(mu_);
    json out = json::array();
    for (auto& [id, a] : approvals_)
        if (a.status == "pending" && (conv.empty() || a.conv == conv) && (conv.empty() ? (owner || a.user_id == user_id) : true))
            out.push_back({{"id", id}, {"conversation_id", a.conv}, {"agent_id", a.agent_id}, {"server", a.label}, {"tool", a.tool},
                           {"user_id", a.user_id}, {"can_decide", owner || a.user_id == user_id}});
    return out;
}

bool Mcp::decide(const std::string& id, const std::string& user_id, const std::string& user_name, bool owner, bool approve, std::string& error) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = approvals_.find(id);
    if (it == approvals_.end() || it->second.status != "pending") { error = "This request is no longer waiting."; return false; }
    if (!owner && it->second.user_id != user_id) { error = "Only the person the ghost works for (or the owner) can answer this."; return false; }
    it->second.status = approve ? "approved" : "declined";
    it->second.by = user_name;
    store_.conv_append(it->second.conv, {{"t", "msg"}, {"from", "system"}, {"approval_result", id},
                                         {"text", std::string(approve ? "✓ " : "✗ ") + user_name + (approve ? " approved " : " declined ") + it->second.label + " › " + it->second.tool}});
    cv_.notify_all();
    return true;
}

}  // namespace pc2
