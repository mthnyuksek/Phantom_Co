#include "store.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "embedded.hpp"

namespace pc2 {

static constexpr int kVaultVersion = 2;

// ------------------------------------------------------------------- config
fs::path default_vault_path() {
    PWSTR docs = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        base = docs;
        CoTaskMemFree(docs);
    } else {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        base = n ? fs::path(buf) / L"Documents" : fs::path(L"C:\\");
    }
    // Older versions used Documents\PC2_Vault (normally saved in node.json anyway): keep using it when it is there.
    std::error_code ec;
    if (fs::exists(base / L"PC2_Vault" / L"vault.json", ec) && !fs::exists(base / L"Phantom_Vault", ec)) return base / L"PC2_Vault";
    return base / L"Phantom_Vault";
}

fs::path vault_target(const fs::path& picked) {
    std::error_code ec;
    if (!fs::exists(picked, ec) || fs::exists(picked / L"vault.json", ec)) return picked;
    if (fs::is_directory(picked, ec) && fs::directory_iterator(picked, ec) == fs::directory_iterator()) return picked;
    return picked / L"Phantom_Vault";
}

static std::wstring norm_path(const fs::path& p) {
    std::wstring s = p.lexically_normal().make_preferred().wstring();
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    for (auto& ch : s) ch = towlower(ch);
    return s;
}
bool same_path(const fs::path& a, const fs::path& b) { return norm_path(a) == norm_path(b); }
bool path_inside(const fs::path& child, const fs::path& parent) {
    std::wstring c = norm_path(child), p = norm_path(parent);
    if (p.empty() || p.back() != L'\\') p += L'\\';
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0;
}

fs::path config_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path base = n ? fs::path(buf) : fs::temp_directory_path();
    fs::path dir = base / L"Phantom_Co", old = base / L"PC2";
    // Older versions kept accounts and settings in %LOCALAPPDATA%\PC2: move that folder once. When it cannot be
    // moved (a file in it is open), keep using it so nobody loses their accounts.
    static const fs::path chosen = [&] {
        std::error_code ec;
        if (fs::exists(dir, ec) || !fs::exists(old, ec)) return dir;
        fs::rename(old, dir, ec);
        return ec ? old : dir;
    }();
    return chosen;
}
fs::path config_file_path() { return config_dir() / L"node.json"; }

Config load_config(const fs::path& file) {
    Config c;
    c.file = file.empty() ? config_file_path() : file;
    json j = read_json(c.file);
    c.vault = j.contains("vault_path") && j["vault_path"].is_string() ? P(j["vault_path"].get<std::string>()) : default_vault_path();
    if (j.contains("port") && j["port"].is_number_integer()) c.port = j["port"].get<int>();
    if (j.contains("share") && j["share"].is_boolean()) c.share = j["share"].get<bool>();
    if (j.contains("worker_url") && j["worker_url"].is_string()) c.worker_url = j["worker_url"].get<std::string>();
    if (j.contains("worker_token") && j["worker_token"].is_string()) c.worker_token = j["worker_token"].get<std::string>();
    if (j.contains("check_updates") && j["check_updates"].is_boolean()) c.check_updates = j["check_updates"].get<bool>();
    if (!fs::exists(c.file)) save_config(c);
    return c;
}
void save_config(const Config& c) {
    json j = {{"vault_path", U8(c.vault)}, {"port", c.port}, {"share", c.share}, {"check_updates", c.check_updates}};
    if (!c.worker_url.empty() && !c.worker_token.empty()) { j["worker_url"] = c.worker_url; j["worker_token"] = c.worker_token; }
    write_json(c.file, j);
}

// ------------------------------------------------------------ field tables
static const std::vector<Field> kAgentF = {
    {"name", "name", 'S', ""},          {"role", "role", 'S', ""},          {"reports_to", "reports_to", 'S', ""},
    {"provider", "provider", 'S', "claude"}, {"model", "model", 'S', ""},   {"form", "form", 'S', "classic"},
    {"color", "color", 'S', "#b69cff"}, {"enabled", "enabled", 'B', "true"}, {"timeout_min", "timeout_min", 'I', "30"},
    {"thinking", "thinking", 'S', ""},  // OpenCode reasoning effort: "" | low | medium | high
    {"memory", "memory", 'B', "false"},  // keeps notes between chats (Memory/<agent>/<user>.md)
    {"skill", "skills", 'L', ""},       {"created", "created", 'S', ""}};
static const std::vector<Field> kSkillF = {
    {"name", "name", 'S', ""}, {"description", "description", 'S', ""}, {"tools", "tools", 'C', ""},
    {"dirs", "dirs", 'C', ""}, {"min_role", "min_role", 'S', "editor"}, {"enabled", "enabled", 'B', "true"},
    {"default", "default", 'B', "false"}, {"projects", "projects", 'C', ""},  // projects: empty = every project
    {"mcp", "mcp", 'C', ""}};  // MCP servers (Settings > MCP) this skill opens
static const std::vector<Field> kRuleF = {{"name", "name", 'S', ""}, {"enabled", "enabled", 'B', "true"}};
static const std::vector<Field> kProjectF = {{"name", "name", 'S', ""}, {"created", "created", 'S', ""}};
static const std::vector<Field> kTaskF = {
    {"name", "title", 'S', ""}, {"agent", "agent_id", 'S', ""}, {"project", "project_id", 'S', ""},
    {"created_by", "created_by", 'S', ""}, {"created", "created", 'S', ""}, {"updated", "updated", 'S', ""},
    {"conversation", "conversation_id", 'S', ""},
    // wake-up schedule: wake = "" | "interval" | "daily"; wake_last = unix seconds of the last wake-up (or of the moment it was set)
    {"wake", "wake", 'S', ""}, {"wake_every", "wake_every", 'I', "0"}, {"wake_at", "wake_at", 'S', ""},
    {"wake_on", "wake_on", 'B', "false"}, {"wake_last", "wake_last", 'S', ""}};
static const std::vector<Field> kTemplateF = {  // task templates: a ready-made task, body = task details
    {"name", "name", 'S', ""}, {"agent", "agent_id", 'S', ""}, {"project", "project_id", 'S', ""},
    {"created_by", "created_by", 'S', ""}, {"created", "created", 'S', ""},
    {"wake", "wake", 'S', ""}, {"wake_every", "wake_every", 'I', "0"}, {"wake_at", "wake_at", 'S', ""}};
static const std::vector<Field> kUserF = {
    {"name", "name", 'S', ""}, {"form", "form", 'S', "classic"}, {"color", "color", 'S', "#9ff0d4"},
    {"created", "created", 'S', ""}, {"disabled", "disabled", 'B', "false"}};
static const std::vector<Field> kRunF = {
    {"kind", "kind", 'S', ""}, {"task", "task_id", 'S', ""}, {"agent", "agent_id", 'S', ""}, {"user", "user_id", 'S', ""},
    {"provider", "provider", 'S', ""}, {"model", "model", 'S', ""}, {"conversation", "conversation_id", 'S', ""},
    {"status", "status", 'S', ""}, {"created", "created", 'S', ""}, {"started", "started", 'S', ""}, {"ended", "ended", 'S', ""},
    {"exit_code", "exit_code", 'I', "0"}, {"tokens_in", "tokens_in", 'I', "0"}, {"tokens_out", "tokens_out", 'I', "0"},
    {"tokens_cached", "tokens_cached", 'I', "0"}, {"cost", "cost", 'S', ""}, {"error", "error", 'S', ""}};

static std::optional<json> read_entity(const fs::path& p, const std::vector<Field>& f, const char* body) {
    std::string text;
    if (!read_file(p, text)) return std::nullopt;
    json j = md_to_json(md_parse(text), f, body);
    j["id"] = U8(p.stem());
    return j;
}
static void write_entity(const fs::path& p, const json& j, const std::vector<Field>& f, const char* body) {
    write_file_atomic(p, md_serialize(json_to_md(j, f, body)));
}
static std::vector<json> read_dir(const fs::path& dir, const std::vector<Field>& f, const char* body) {
    std::vector<json> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != L".md") continue;
        if (auto j = read_entity(e.path(), f, body)) out.push_back(std::move(*j));
    }
    return out;
}
static void sort_by(std::vector<json>& v, const char* key) {
    std::sort(v.begin(), v.end(), [key](const json& a, const json& b) {
        std::string x = a.value(key, ""), y = b.value(key, "");
        return x == y ? a.value("id", "") < b.value("id", "") : x < y;
    });
}

// -------------------------------------------------------------- templates
static std::vector<json> embedded_skills() {
    std::vector<json> out;
    for (size_t i = 0; i < kEmbeddedCount; ++i) {
        const EmbeddedFile& f = kEmbedded[i];
        if (std::string(f.group) != "skills") continue;
        std::string path = f.path;
        if (path.size() < 3 || path.substr(path.size() - 3) != ".md") continue;
        json j = md_to_json(md_parse(std::string(reinterpret_cast<const char*>(f.data), f.size)), kSkillF, "body");
        j["id"] = path.substr(0, path.size() - 3);
        out.push_back(std::move(j));
    }
    return out;
}
static void install_skill(const fs::path& root, const json& s) {
    fs::path p = root / L"Skills" / P(s["id"].get<std::string>() + ".md");
    std::error_code ec;
    if (!fs::exists(p, ec)) write_entity(p, s, kSkillF, "body");
}

// ------------------------------------------------------------- vault init
static void hide(const fs::path& p) { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_HIDDEN); }

static void migrate_v1(const fs::path& root) {
    std::error_code ec;
    fs::path archive = root / L"Archive_V1";
    fs::create_directories(archive / L"Agents", ec);
    fs::create_directories(root / L"Skills", ec);
    for (const json& s : embedded_skills()) if (s.value("default", false)) install_skill(root, s);

    for (auto& e : fs::directory_iterator(root / L"Agents", ec)) {
        if (e.path().extension() != L".json") continue;
        json a = read_json(e.path(), json());
        if (!a.is_object() || !a.contains("id")) continue;
        std::string access = a.value("access", "workspace");
        json skills = json::array({"read_files"});
        if (access != "read_only") skills.push_back("write_files");
        if (access == "workspace_shell") {
            skills.push_back("shell_commands");
            for (const json& s : embedded_skills()) if (s["id"] == "shell_commands") install_skill(root, s);
        }
        json n = {{"id", a["id"]}, {"name", a.value("name", "Ghost")}, {"role", a.value("role", "")}, {"provider", a.value("provider", "claude")},
                  {"model", a.value("model", "")}, {"form", a.value("form", "classic")}, {"color", a.value("color", "#b69cff")},
                  {"enabled", true}, {"timeout_min", a.value("timeout_min", 30)}, {"skills", skills},
                  {"created", a.value("created", now_iso())}, {"persona", a.value("prompt", "")}};
        write_entity(root / L"Agents" / P(U8(e.path().stem()) + ".md"), n, kAgentF, "persona");
        fs::rename(e.path(), archive / L"Agents" / e.path().filename(), ec);
    }
    if (fs::exists(root / L"Tasks", ec)) fs::rename(root / L"Tasks", archive / L"Tasks", ec);
    fs::create_directories(root / L"Tasks", ec);
    std::string readme = "Version 1 data (JSON agents, old task runs) was moved here when the Vault was upgraded to version 2. Agents were converted; old task runs are kept only as files.\n";
    write_file_atomic(archive / L"README.txt", readme);
}

void init_vault(const fs::path& root) {
    std::error_code ec;
    fs::path marker = root / L"vault.json";
    bool exists = fs::exists(root, ec);
    if (exists && !fs::is_directory(root, ec)) throw std::runtime_error("Vault path is a file: " + U8(root));

    bool fresh = false, migrated = false;
    if (exists && fs::exists(marker, ec)) {
        json m = read_json(marker);
        int v = m.value("vault_version", 0);
        if (v < 1 || v > kVaultVersion) throw std::runtime_error("Unknown Vault version " + std::to_string(v) + " in " + U8(marker));
        if (v == 1) {
            migrate_v1(root);
            m["vault_version"] = kVaultVersion;
            write_json(marker, m);
            migrated = true;
        }
    } else {
        if (exists && !fs::is_empty(root, ec)) throw std::runtime_error("Folder is not empty and is not a Phantom Co. Vault: " + U8(root));
        fs::create_directories(root, ec);
        if (ec) throw std::runtime_error("Cannot create Vault folder: " + U8(root) + " (" + ec.message() + ")");
        write_json(marker, json{{"vault_version", kVaultVersion}, {"vault_id", new_id()}, {"created", now_iso()}, {"settings", json::object()}});
        fresh = true;
    }

    for (const wchar_t* d : {L"Company", L"Users", L"Agents", L"Skills", L"Rules", L"Projects", L"Tasks", L"Task_Templates", L"Scripts", L"Script_Runs", L"Conversations", L"Runs", L"Shared_Files", L"Memory"})
        fs::create_directories(root / d, ec);

    for (size_t i = 0; i < kEmbeddedCount; ++i) {  // starting files from the template; existing files are kept
        const EmbeddedFile& f = kEmbedded[i];
        if (std::string(f.group) != "vault") continue;
        fs::path rel = P(f.path);
        if (rel.filename() == L".gitkeep") continue;
        fs::path target = root / rel;
        if (fs::exists(target, ec) && !(migrated && rel.filename() == L"README.md")) continue;
        write_file_atomic(target, std::string(reinterpret_cast<const char*>(f.data), f.size));
    }
    if (fresh) for (const json& s : embedded_skills()) if (s.value("default", false)) install_skill(root, s);
    // The guide's knowledge follows the program: once installed, "app_guide" is rewritten from the built-in copy on every start.
    for (const json& s : embedded_skills())
        if (s["id"] == "app_guide" && fs::exists(root / L"Skills" / L"app_guide.md", ec)) write_entity(root / L"Skills" / L"app_guide.md", s, kSkillF, "body");
}

// -------------------------------------------------------------------- store
// Two Phantom Co. windows on one Vault would overwrite each other's files: every open Vault holds a hidden lock file
// open without sharing (deleted when the program closes, also after a crash because Windows closes the handle).
static void* lock_vault(const fs::path& root) {
    HANDLE h = CreateFileW((root / L".phantom_lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error(GetLastError() == ERROR_SHARING_VIOLATION
                                     ? "This Vault is already open in another Phantom Co. window."
                                     : "The Vault folder cannot be locked (no write access?).");
    return h;
}

Store::Store(const fs::path& root) : root_(root) {
    init_vault(root_);
    lock_ = lock_vault(root_);
    load_run_index();
}
Store::~Store() {
    if (lock_) CloseHandle(lock_);
}

fs::path Store::root() const { std::lock_guard<std::recursive_mutex> l(mu_); return root_; }
fs::path Store::files_root() const { return root() / L"Shared_Files"; }
fs::path Store::project_dir(const std::string& id) const { return root() / L"Projects" / P(id); }

void Store::switch_root(const fs::path& new_root) {
    init_vault(new_root);
    std::lock_guard<std::recursive_mutex> l(mu_);
    void* lock = same_path(new_root, root_) ? nullptr : lock_vault(new_root);
    if (lock) { if (lock_) CloseHandle(lock_); lock_ = lock; }
    root_ = new_root;
    load_run_index();
}

std::string Store::unique_id(const fs::path& dir, const std::string& name, bool folder, const char* ext) {
    std::string base = slugify(name, 32);
    std::error_code ec;
    for (int i = 1; i < 10000; ++i) {
        std::string id = i == 1 ? base : base + "_" + std::to_string(i);
        if (!fs::exists(dir / P(folder ? id : id + ext), ec)) return id;
    }
    return base + "_" + new_id().substr(18);
}

json Store::company() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json c = read_json(root_ / L"Company" / L"company.json");
    if (!c.contains("name")) c["name"] = "Phantom Co.";
    if (!c.contains("goal")) c["goal"] = "";
    return c;
}
void Store::set_company(const json& c) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    write_json(root_ / L"Company" / L"company.json", json{{"name", c.value("name", "Phantom Co.")}, {"goal", c.value("goal", "")}});
}

json Store::settings() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json m = read_json(root_ / L"vault.json");
    json s = {{"max_hops", 8}, {"default_model", ""}, {"max_queue_per_user", 10}, {"max_concurrent", 1}};
    if (m.contains("settings") && m["settings"].is_object())
        for (auto& [k, v] : m["settings"].items()) if (s.contains(k)) s[k] = v;
    return s;
}
void Store::set_settings(const json& patch) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json m = read_json(root_ / L"vault.json");
    json s = settings();
    for (auto& [k, v] : patch.items()) if (s.contains(k)) s[k] = v;
    m["settings"] = s;
    write_json(root_ / L"vault.json", m);
}
int Store::setting_int(const char* name) { return settings().value(name, 0); }

// ------------------------------------------------------------------ simple
#define SIMPLE_ENTITY(Plural, Singular, Folder, Fields, Body, SortKey)                                         \
    std::vector<json> Store::Plural() {                                                                         \
        std::lock_guard<std::recursive_mutex> l(mu_);                                                           \
        auto v = read_dir(root_ / Folder, Fields, Body);                                                        \
        sort_by(v, SortKey);                                                                                    \
        return v;                                                                                               \
    }                                                                                                           \
    std::optional<json> Store::Singular(const std::string& id) {                                                \
        if (!valid_id(id)) return std::nullopt;                                                                 \
        std::lock_guard<std::recursive_mutex> l(mu_);                                                           \
        return read_entity(root_ / Folder / P(id + ".md"), Fields, Body);                                       \
    }                                                                                                           \
    bool Store::remove_##Singular(const std::string& id) {                                                      \
        if (!valid_id(id)) return false;                                                                        \
        std::lock_guard<std::recursive_mutex> l(mu_);                                                           \
        std::error_code ec;                                                                                     \
        return fs::remove(root_ / Folder / P(id + ".md"), ec);                                                  \
    }

SIMPLE_ENTITY(users, user, L"Users", kUserF, nullptr, "created")
SIMPLE_ENTITY(agents, agent, L"Agents", kAgentF, "persona", "created")
SIMPLE_ENTITY(skills, skill, L"Skills", kSkillF, "body", "name")
SIMPLE_ENTITY(rules, rule, L"Rules", kRuleF, "body", "name")
SIMPLE_ENTITY(task_templates, task_template, L"Task_Templates", kTemplateF, "description", "name")

json Store::save_user(json u) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!u.contains("id") || u["id"].get<std::string>().empty()) {
        u["id"] = unique_id(root_ / L"Users", u.value("name", "user"), false);
        u["created"] = now_iso();
    }
    write_entity(root_ / L"Users" / P(u["id"].get<std::string>() + ".md"), u, kUserF, nullptr);
    return u;
}
json Store::save_agent(json a) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!a.contains("id") || a["id"].get<std::string>().empty()) {
        a["id"] = unique_id(root_ / L"Agents", a.value("name", "ghost"), false);
        a["created"] = now_iso();
    }
    write_entity(root_ / L"Agents" / P(a["id"].get<std::string>() + ".md"), a, kAgentF, "persona");
    return a;
}
json Store::save_skill(json s) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!s.contains("id") || s["id"].get<std::string>().empty()) s["id"] = unique_id(root_ / L"Skills", s.value("name", "skill"), false);
    write_entity(root_ / L"Skills" / P(s["id"].get<std::string>() + ".md"), s, kSkillF, "body");
    return s;
}
json Store::save_rule(json r) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!r.contains("id") || r["id"].get<std::string>().empty()) r["id"] = unique_id(root_ / L"Rules", r.value("name", "rule"), false);
    write_entity(root_ / L"Rules" / P(r["id"].get<std::string>() + ".md"), r, kRuleF, "body");
    return r;
}

json Store::save_task_template(json t) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!t.contains("id") || t["id"].get<std::string>().empty()) {
        t["id"] = unique_id(root_ / L"Task_Templates", t.value("name", "template"), false);
        t["created"] = now_iso();
    }
    write_entity(root_ / L"Task_Templates" / P(t["id"].get<std::string>() + ".md"), t, kTemplateF, "description");
    return t;
}

// ----------------------------------------------------------------- scripts
std::vector<json> Store::scripts() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<json> v;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Scripts", ec)) {
        if (!e.is_regular_file() || e.path().extension() != L".json") continue;
        json s = read_json(e.path(), json());
        if (!s.is_object()) continue;
        s["id"] = U8(e.path().stem());
        v.push_back(s);
    }
    sort_by(v, "name");
    return v;
}
std::optional<json> Store::script(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    fs::path f = root_ / L"Scripts" / P(id + ".json");
    if (!fs::exists(f, ec)) return std::nullopt;
    json s = read_json(f, json());
    if (!s.is_object()) return std::nullopt;
    s["id"] = id;
    return s;
}
json Store::save_script(json s) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    fs::create_directories(root_ / L"Scripts", ec);
    if (!s.contains("id") || !s["id"].is_string() || s["id"].get<std::string>().empty()) {
        s["id"] = unique_id(root_ / L"Scripts", s.value("name", "script"), false, ".json");
        s["created"] = now_iso();
    }
    s["updated"] = now_iso();
    write_json(root_ / L"Scripts" / P(s["id"].get<std::string>() + ".json"), s);
    return s;
}
bool Store::remove_script(const std::string& id) {
    if (!valid_id(id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    return fs::remove(root_ / L"Scripts" / P(id + ".json"), ec);
}

void Store::script_run_save(const json& st) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string id = st.value("id", "");
    if (!valid_id(id)) return;
    std::error_code ec;
    fs::create_directories(root_ / L"Script_Runs", ec);
    write_json(root_ / L"Script_Runs" / P(id + ".json"), st);
}
std::optional<json> Store::script_run_get(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    json st = read_json(root_ / L"Script_Runs" / P(id + ".json"), json());
    if (!st.is_object() || !st.contains("id")) return std::nullopt;
    return st;
}
std::vector<json> Store::script_runs(const std::string& script_id, size_t limit) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<std::string> ids;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Script_Runs", ec))
        if (e.is_regular_file() && e.path().extension() == L".json") ids.push_back(U8(e.path().stem()));
    std::sort(ids.rbegin(), ids.rend());  // ULIDs: newest first
    std::vector<json> out;
    for (const auto& id : ids) {
        if (out.size() >= limit) break;
        auto st = script_run_get(id);
        if (st && (script_id.empty() || st->value("script_id", "") == script_id)) out.push_back(*st);
    }
    return out;
}
void Store::script_run_log(const std::string& id, const json& ev) {
    if (!valid_id(id)) return;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::ofstream f(root_ / L"Script_Runs" / P(id + ".jsonl"), std::ios::app | std::ios::binary);
    f << dumps(ev) << "\n";
}
std::vector<json> Store::script_run_events(const std::string& id, size_t limit) {
    std::vector<json> out;
    if (!valid_id(id)) return out;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string text;
    if (!read_file(root_ / L"Script_Runs" / P(id + ".jsonl"), text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        json ev = json::parse(text.substr(pos, e - pos), nullptr, false);
        if (ev.is_object()) out.push_back(std::move(ev));
        pos = e + 1;
    }
    if (out.size() > limit) out.erase(out.begin(), out.end() - limit);
    return out;
}

std::vector<json> Store::skill_templates() { return embedded_skills(); }
std::optional<json> Store::install_skill_template(const std::string& id) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    for (const json& s : embedded_skills())
        if (s["id"] == id) {
            install_skill(root_, s);
            return skill(id);
        }
    return std::nullopt;
}

// ---------------------------------------------------------------- projects
std::vector<json> Store::projects() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<json> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Projects", ec)) {
        if (!e.is_directory(ec)) continue;
        if (auto p = project(U8(e.path().filename()))) out.push_back(std::move(*p));
    }
    sort_by(out, "name");
    return out;
}
std::optional<json> Store::project(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    fs::path meta = root_ / L"Projects" / P(id) / L".Phantom";
    auto p = read_entity(meta / L"project.md", kProjectF, "summary");
    if (!p) return std::nullopt;
    (*p)["id"] = id;  // the folder name, not the stem of project.md
    json notes = json::array();
    std::error_code ec;
    for (auto& n : read_dir(meta / L"Notes", kRuleF, "body")) notes.push_back(std::move(n));
    (*p)["notes"] = notes;
    json roles = json::object();  // .Phantom/Roles/<agent id>.md: what that agent does in this project
    for (auto& r : read_dir(meta / L"Roles", kRuleF, "body")) roles[r["id"].get<std::string>()] = r.value("body", "");
    (*p)["roles"] = roles;
    return p;
}
json Store::save_project(json p) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    if (!p.contains("id") || p["id"].get<std::string>().empty()) {
        p["id"] = unique_id(root_ / L"Projects", p.value("name", "project"), true);
        p["created"] = now_iso();
    }
    fs::path dir = root_ / L"Projects" / P(p["id"].get<std::string>());
    fs::create_directories(dir / L".Phantom" / L"Notes", ec);
    hide(dir / L".Phantom");
    write_file_atomic(dir / L".Phantom" / L"project.md", md_serialize(json_to_md(p, kProjectF, "summary")));
    return p;
}
bool Store::remove_project(const std::string& id) {
    if (!valid_id(id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    return fs::remove_all(root_ / L"Projects" / P(id) / L".Phantom", ec) > 0;
}
json Store::save_note(const std::string& project_id, json n) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    fs::path dir = root_ / L"Projects" / P(project_id) / L".Phantom" / L"Notes";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!n.contains("id") || n["id"].get<std::string>().empty()) n["id"] = unique_id(dir, n.value("name", "note"), false);
    write_entity(dir / P(n["id"].get<std::string>() + ".md"), n, kRuleF, "body");
    return n;
}
bool Store::set_project_role(const std::string& project_id, const std::string& agent_id, const std::string& text) {
    if (!valid_id(project_id) || !valid_id(agent_id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    fs::path dir = root_ / L"Projects" / P(project_id) / L".Phantom" / L"Roles";
    std::error_code ec;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) { fs::remove(dir / P(agent_id + ".md"), ec); return true; }
    fs::create_directories(dir, ec);
    write_entity(dir / P(agent_id + ".md"), json{{"name", agent_id}, {"body", text}}, kRuleF, "body");
    return true;
}
bool Store::remove_note(const std::string& project_id, const std::string& note_id) {
    if (!valid_id(project_id) || !valid_id(note_id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    return fs::remove(root_ / L"Projects" / P(project_id) / L".Phantom" / L"Notes" / P(note_id + ".md"), ec);
}

// ------------------------------------------------------------------- tasks
std::vector<json> Store::tasks() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto v = read_dir(root_ / L"Tasks", kTaskF, "description");
    std::sort(v.begin(), v.end(), [](const json& a, const json& b) { return a.value("id", "") > b.value("id", ""); });
    return v;
}
std::optional<json> Store::task(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    return read_entity(root_ / L"Tasks" / P(id + ".md"), kTaskF, "description");
}
json Store::save_task(json t) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!t.contains("id") || t["id"].get<std::string>().empty()) {
        t["id"] = new_id();
        t["created"] = now_iso();
    }
    t["updated"] = now_iso();
    write_entity(root_ / L"Tasks" / P(t["id"].get<std::string>() + ".md"), t, kTaskF, "description");
    return t;
}
bool Store::remove_task(const std::string& id) {
    if (!valid_id(id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    return fs::remove(root_ / L"Tasks" / P(id + ".md"), ec);
}

// ----------------------------------------------------------- conversations
std::string Store::conv_create(json meta) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string id = new_id();
    meta["t"] = "meta";
    meta["created"] = now_iso();
    write_file_atomic(root_ / L"Conversations" / P(id + ".jsonl"), dumps(meta) + "\n");
    return id;
}

void Store::conv_append(const std::string& id, json line) {
    if (!valid_id(id)) return;
    std::lock_guard<std::recursive_mutex> l(mu_);
    if (!line.contains("ts")) line["ts"] = now_iso();
    std::ofstream f(root_ / L"Conversations" / P(id + ".jsonl"), std::ios::app | std::ios::binary);
    f << dumps(line) << "\n";
    f.close();
    if (append_hook_) append_hook_(id, line);
}

std::optional<json> Store::conv_load(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string text;
    if (!read_file(root_ / L"Conversations" / P(id + ".jsonl"), text)) return std::nullopt;
    json meta = json::object(), msgs = json::array(), sessions = json::object();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        json j = json::parse(text.substr(pos, e - pos), nullptr, false);
        pos = e + 1;
        if (!j.is_object()) continue;
        std::string t = j.value("t", "");
        if (t == "meta") { for (auto& [k, v] : j.items()) if (k != "t") meta[k] = v; }
        else if (t == "title") meta["title"] = j.value("title", "");
        else if (t == "share") meta["shared"] = j.value("shared", false);
        else if (t == "project") meta["project_id"] = j.value("project_id", "");
        else if (t == "session") sessions[j.value("agent", "")] = j.value("sid", "");
        else if (t == "reset") { sessions = json::object(); j["from"] = "system"; j["text"] = j.value("text", "New session"); msgs.push_back(j); }
        else if (t == "msg") msgs.push_back(j);
    }
    meta["id"] = id;
    return json{{"meta", meta}, {"messages", msgs}, {"sessions", sessions}};
}

std::vector<json> Store::conv_list() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<json> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Conversations", ec)) {
        if (e.path().extension() != L".jsonl") continue;
        auto c = conv_load(U8(e.path().stem()));
        if (!c) continue;
        json m = (*c)["meta"];
        const json& msgs = (*c)["messages"];
        m["count"] = msgs.size();
        m["updated"] = msgs.empty() ? m.value("created", "") : msgs.back().value("ts", "");
        m["preview"] = msgs.empty() ? "" : truncate_utf8(msgs.back().value("text", ""), 120);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a.value("updated", "") > b.value("updated", ""); });
    return out;
}

std::vector<std::string> Store::conv_ids() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Conversations", ec))
        if (e.path().extension() == L".jsonl") out.push_back(U8(e.path().stem()));
    return out;
}

// ----------------------------------------------------------------- budgets
json Store::budgets() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    json b = read_json(root_ / L"Company" / L"budgets.json");
    return b.is_object() ? b : json::object();
}
void Store::save_budgets(const json& b) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    write_json(root_ / L"Company" / L"budgets.json", b);
}

// ------------------------------------------------------------------ memory
static std::string memory_clean(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (space && (out.empty() || out.back() == ' ')) continue;
        out += space ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return truncate_utf8(out, kMaxMemoryText);
}

std::vector<std::string> Store::memory(const std::string& agent_id, const std::string& user_id) {
    std::vector<std::string> out;
    if (!valid_id(agent_id) || !valid_id(user_id)) return out;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string text;
    if (!read_file(root_ / L"Memory" / P(agent_id) / P(user_id + ".md"), text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (line.rfind("- ", 0) == 0) line.erase(0, 2);
        line = memory_clean(line);
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

void Store::memory_write(const std::string& agent_id, const std::string& user_id, const std::vector<std::string>& notes) {
    const fs::path dir = root_ / L"Memory" / P(agent_id);
    std::error_code ec;
    if (notes.empty()) { fs::remove(dir / P(user_id + ".md"), ec); return; }
    fs::create_directories(dir, ec);
    std::string text;
    for (auto& n : notes) text += "- " + n + "\n";
    write_file_atomic(dir / P(user_id + ".md"), text);
}

bool Store::memory_add(const std::string& agent_id, const std::string& user_id, const std::string& text) {
    if (!valid_id(agent_id) || !valid_id(user_id)) return false;
    const std::string note = memory_clean(text);
    if (note.empty()) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto notes = memory(agent_id, user_id);
    if (std::find(notes.begin(), notes.end(), note) != notes.end()) return false;
    notes.push_back(note);
    while (notes.size() > kMaxMemory) notes.erase(notes.begin());
    memory_write(agent_id, user_id, notes);
    return true;
}

std::string Store::memory_remove(const std::string& agent_id, const std::string& user_id, size_t number) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto notes = memory(agent_id, user_id);
    if (number < 1 || number > notes.size()) return "";
    std::string gone = notes[number - 1];
    notes.erase(notes.begin() + (long)(number - 1));
    memory_write(agent_id, user_id, notes);
    return gone;
}

void Store::memory_clear(const std::string& agent_id, const std::string& user_id) {
    if (!valid_id(agent_id) || !valid_id(user_id)) return;
    std::lock_guard<std::recursive_mutex> l(mu_);
    memory_write(agent_id, user_id, {});
}

void Store::memory_forget_agent(const std::string& agent_id) {
    if (!valid_id(agent_id)) return;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    fs::remove_all(root_ / L"Memory" / P(agent_id), ec);
}

void Store::memory_forget_user(const std::string& user_id) {
    if (!valid_id(user_id)) return;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Memory", ec)) {
        std::error_code e2;
        fs::remove(e.path() / P(user_id + ".md"), e2);
    }
}

bool Store::conv_delete(const std::string& id) {
    if (!valid_id(id)) return false;
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::error_code ec;
    return fs::remove(root_ / L"Conversations" / P(id + ".jsonl"), ec);
}

// -------------------------------------------------------------------- runs
void Store::load_run_index() {
    run_index_.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / L"Runs", ec)) {
        if (e.path().extension() != L".md") continue;
        if (auto r = read_entity(e.path(), kRunF, "result")) {
            r->erase("result");
            run_index_[(*r)["id"].get<std::string>()] = *r;
        }
    }
}

void Store::run_save(const json& rec) {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::string id = rec["id"];
    write_entity(root_ / L"Runs" / P(id + ".md"), rec, kRunF, "result");
    json idx = rec;
    idx.erase("result");
    run_index_[id] = idx;
}
std::optional<json> Store::run_get(const std::string& id) {
    if (!valid_id(id)) return std::nullopt;
    std::lock_guard<std::recursive_mutex> l(mu_);
    return read_entity(root_ / L"Runs" / P(id + ".md"), kRunF, "result");
}
std::map<std::string, long long> Store::tokens_today() {
    std::map<std::string, long long> out;
    std::time_t now = std::time(nullptr);
    std::tm lt = *std::localtime(&now);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    long long day_start = (long long)std::mktime(&lt);
    for (auto& r : runs()) {  // newest first; run times are UTC
        std::string created = r.value("created", "");
        std::tm ut = {};
        if (created.size() < 19 || sscanf(created.c_str(), "%d-%d-%dT%d:%d:%d", &ut.tm_year, &ut.tm_mon, &ut.tm_mday, &ut.tm_hour, &ut.tm_min, &ut.tm_sec) != 6) continue;
        ut.tm_year -= 1900;
        ut.tm_mon -= 1;
        if ((long long)_mkgmtime(&ut) < day_start) break;
        out[r.value("agent_id", "")] += r.value("tokens_in", 0LL) + r.value("tokens_out", 0LL);
    }
    return out;
}

std::vector<json> Store::runs() {
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::vector<json> out;
    for (auto it = run_index_.rbegin(); it != run_index_.rend(); ++it) out.push_back(it->second);
    return out;
}
fs::path Store::run_events_path(const std::string& id) { return root() / L"Runs" / P(id + ".jsonl"); }

void Store::recover_interrupted() {
    for (json r : runs()) {
        std::string s = r.value("status", "");
        if (s != "running" && s != "queued") continue;
        if (auto full = run_get(r["id"])) {
            (*full)["status"] = "interrupted";
            (*full)["ended"] = now_iso();
            (*full)["error"] = "The program was closed while this run was active.";
            run_save(*full);
        }
    }
}

}  // namespace pc2
