#include "providers.hpp"

#include <windows.h>

#include <algorithm>
#include <ctime>

#include "http_client.hpp"

namespace pc2 {

static std::string env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n ? Narrow(std::wstring(buf, n)) : "";
}

static std::string find_exe(const std::vector<std::wstring>& names, const std::vector<std::string>& fallbacks) {
    for (auto& n : names) {
        wchar_t buf[MAX_PATH * 2];
        DWORD r = SearchPathW(nullptr, n.c_str(), nullptr, MAX_PATH * 2, buf, nullptr);
        if (r > 0 && r < MAX_PATH * 2) return Narrow(std::wstring(buf, r));
    }
    for (auto& f : fallbacks) {
        std::error_code ec;
        if (!f.empty() && fs::exists(P(f), ec)) return f;
    }
    return "";
}

static std::string claude_path() {
    return find_exe({L"claude.exe", L"claude.cmd"}, {env(L"USERPROFILE") + "\\.local\\bin\\claude.exe"});
}
static std::string codex_path() {
    return find_exe({L"codex.cmd", L"codex.exe"}, {env(L"APPDATA") + "\\npm\\codex.cmd"});
}
static std::string gemini_path() {
    return find_exe({L"gemini.cmd", L"gemini.exe"}, {env(L"APPDATA") + "\\npm\\gemini.cmd"});
}
static std::string opencode_path() {
    return find_exe({L"opencode.cmd", L"opencode.exe"}, {env(L"APPDATA") + "\\npm\\opencode.cmd"});
}

const std::vector<OpenCodeService>& opencode_services() {
    // A new service is one line: OpenCode's provider id (models.dev), its key variable, a tool-calling default model and a
    // URL that answers 401 for a wrong key. Checked 2026-11-06 with OpenCode 1.19.5.
    static const std::vector<OpenCodeService> list = {
        {"nvidia", "NVIDIA", "nvapi-", "NVIDIA_API_KEY", "nvidia/nvidia/nemotron-3.5-lightning-30b-a3b", ""},
        {"openrouter", "OpenRouter", "sk-or-", "OPENROUTER_API_KEY", "openrouter/nvidia/nemotron-3.5-lightning:free", "https://openrouter.ai/api/v1/key"},
        {"groq", "Groq", "gsk_", "GROQ_API_KEY", "groq/openai/gpt-oss-120b", "https://api.groq.com/openai/v1/models"},
        {"cerebras", "Cerebras", "csk-", "CEREBRAS_API_KEY", "cerebras/gpt-oss-120b", "https://api.cerebras.ai/v1/models"},
        {"deepseek", "DeepSeek", "", "DEEPSEEK_API_KEY", "deepseek/deepseek-v4-flash", "https://api.deepseek.com/models"},
        {"mistral", "Mistral", "", "MISTRAL_API_KEY", "mistral/mistral-medium-2508", "https://api.mistral.ai/v1/models"},
    };
    return list;
}
const std::vector<OpenCodeLocal>& opencode_locals() {
    static const std::vector<OpenCodeLocal> list = {
        {"lmstudio", "LM Studio", "http://127.0.0.1:1234"},
        {"ollama", "Ollama", "http://127.0.0.1:11434"},
    };
    return list;
}
json opencode_local_models() {
    json out = json::array();
    for (auto& l : opencode_locals()) {
        HttpClient http(l.host, 700, 2000);  // a server that is off fails at once; one that is busy gets two seconds
        auto r = http.Get("/v1/models");
        if (!r || r->status != 200) continue;
        json j = json::parse(r->body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) continue;
        for (auto& m : j["data"])
            if (m.is_object() && m.contains("id") && m["id"].is_string()) {
                const std::string id = m["id"];
                if (id.find("embed") != std::string::npos) continue;  // embedding models cannot chat
                out.push_back({{"id", std::string(l.id) + "/" + id}, {"server", l.label}});
            }
    }
    return out;
}
const OpenCodeService* opencode_service_for_key(const std::string& key) {
    for (auto& s : opencode_services()) if (*s.prefix && key.rfind(s.prefix, 0) == 0) return &s;
    return nullptr;
}
const OpenCodeService* opencode_service(const std::string& id) {
    for (auto& s : opencode_services()) if (id == s.id) return &s;
    return nullptr;
}

std::vector<ProviderInfo> detect_providers() {
    std::string c = claude_path(), x = codex_path(), g = gemini_path(), o = opencode_path();
    return {{"claude", "Claude Code", !c.empty(), c},
            {"codex", "Codex", !x.empty(), x},
            {"gemini", "Gemini CLI", !g.empty(), g},
            {"opencode", "OpenCode", !o.empty(), o},
            {"demo", "Demo (simulated, no subscription used)", true, ""}};
}

std::string provider_exe(const std::string& id) {
    return id == "claude" ? claude_path() : id == "codex" ? codex_path() : id == "gemini" ? gemini_path() : id == "opencode" ? opencode_path() : "";
}

bool provider_known(const std::string& id) { return id == "claude" || id == "codex" || id == "gemini" || id == "opencode" || id == "demo"; }

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

static std::vector<std::string> proxy_args(const RunSpec& rs, const std::string& name) {
    return {"--mcp-proxy", name, "--mcp-port", std::to_string(rs.mcp_port), "--mcp-key", rs.mcp_key};
}

// A TOML string for Codex's -c option: a literal string ('...') unless the text has a quote in it.
static std::string toml_str(const std::string& s) {
    if (s.find('\'') == std::string::npos) return "'" + s + "'";
    return "\"" + replace_all(replace_all(s, "\\", "\\\\"), "\"", "\\\"") + "\"";
}

bool build_command(const RunSpec& rs, ProcessSpec& out, std::string& error) {
    out = {};
    out.cwd = rs.workdir;
    out.env = rs.env;

    if (rs.provider == "claude") {
        out.exe = claude_path();
        if (out.exe.empty()) {
            error = "Claude Code CLI was not found on this PC (is `claude` installed and on PATH?).";
            return false;
        }
        out.stdin_data = rs.prompt;
        // Lean profile: own system prompt, only the tools the skills grant, only our MCP servers, no slash commands.
        out.args = {"-p", "--output-format", "stream-json", "--verbose", "--system-prompt-file", rs.system_file,
                    "--tools", join(rs.tools), "--strict-mcp-config", "--disable-slash-commands",
                    "--setting-sources", "project", "--permission-prompts", "none", "--max-turns", std::to_string(rs.max_turns)};
        std::vector<std::string> allowed = rs.tools;
        if (!rs.mcp.empty()) {
            json servers = json::object();
            for (auto& n : rs.mcp) {
                servers[n] = {{"type", "stdio"}, {"command", rs.mcp_exe}, {"args", proxy_args(rs, n)}};
                allowed.push_back("mcp__" + n);  // every tool of that server
            }
            if (!write_file_atomic(P(rs.mcp_file), json{{"mcpServers", servers}}.dump(2))) { error = "Could not write the MCP settings of the run."; return false; }
            out.args.push_back("--mcp-config");
            out.args.push_back(rs.mcp_file);
            out.env.push_back({"MCP_TIMEOUT", "120000"});        // npx may download a server first
            out.env.push_back({"MCP_TOOL_TIMEOUT", "3600000"});  // a call may wait for a person's approval
        }
        if (!allowed.empty()) { out.args.push_back("--allowedTools"); out.args.push_back(join(allowed)); }
        if (!rs.model.empty()) { out.args.push_back("--model"); out.args.push_back(rs.model); }
        if (!rs.session_id.empty()) { out.args.push_back("--resume"); out.args.push_back(rs.session_id); }
        for (auto& d : rs.add_dirs) { out.args.push_back("--add-dir"); out.args.push_back(d); }
        return true;
    }
    if (rs.provider == "codex") {
        out.exe = codex_path();
        if (out.exe.empty()) {
            error = "Codex CLI was not found on this PC (is `codex` installed and on PATH?).";
            return false;
        }
        // Codex has no system prompt option: the persona travels in front of the message.
        out.stdin_data = rs.system_prompt;
        if (rs.can_write)
            out.stdin_data += "\n\nImages you generate are copied into the Generated_Images folder of your working folder automatically when you"
                              " finish. Do not copy or re-encode them yourself; just name that folder in your answer.";
        out.stdin_data += "\n\n---\n\n" + rs.prompt;
        const std::string sandbox = rs.can_write ? "workspace-write" : "read-only";
        if (rs.session_id.empty()) {
            out.args = {"exec", "--json", "--skip-git-repo-check", "-s", sandbox, "-C", rs.workdir};
            for (auto& d : rs.add_dirs) { out.args.push_back("--add-dir"); out.args.push_back(d); }
        } else {
            out.args = {"exec", "resume", rs.session_id, "--json", "--skip-git-repo-check", "-c", "sandbox_mode=\"" + sandbox + "\""};
        }
        if (!rs.model.empty()) { out.args.push_back("-m"); out.args.push_back(rs.model); }
        // Only our MCP servers: this replaces the mcp_servers of the user's own config.toml (empty = none at all).
        std::string table;
        for (auto& n : rs.mcp) {
            std::string args;
            for (auto& a : proxy_args(rs, n)) args += (args.empty() ? "" : ",") + toml_str(a);
            // Codex asks before every MCP call and `exec` cannot ask anybody ("approval policy is never"): our proxy does
            // the asking (Settings > MCP > Ask me before it acts), so Codex itself approves.
            table += (table.empty() ? "" : ",") + n + "={command=" + toml_str(rs.mcp_exe) + ",args=[" + args +
                     "],startup_timeout_sec=120,tool_timeout_sec=3600,default_tools_approval_mode='approve'}";
        }
        out.args.push_back("-c");
        out.args.push_back("mcp_servers={" + table + "}");
        out.args.push_back("-");  // read the prompt from stdin
        return true;
    }
    if (rs.provider == "gemini") {
        out.exe = gemini_path();
        if (out.exe.empty()) {
            error = "Gemini CLI was not found on this PC (is `gemini` installed and on PATH?).";
            return false;
        }
        // Headless: the prompt comes through stdin ("-p" adds nothing). Lean profile: our own system prompt
        // (GEMINI_SYSTEM_MD = a file), no extensions, no MCP servers. Gemini has no per-tool switch on the command line:
        // the approval mode follows the skills (shell -> yolo, files -> auto_edit, else read-only tools only).
        out.stdin_data = rs.prompt;
        const bool shell = std::find(rs.tools.begin(), rs.tools.end(), "Bash") != rs.tools.end();
        out.args = {"-p", "", "-o", "stream-json", "--skip-trust", "-e", "none", "--approval-mode", shell ? "yolo" : rs.can_write ? "auto_edit" : "default"};
        if (rs.mcp.empty()) { out.args.push_back("--allowed-mcp-server-names"); out.args.push_back("phantom_none"); }
        else {
            // Our servers come from a system settings file of the run; only they are allowed (not the user's own ones).
            json servers = json::object();
            for (auto& n : rs.mcp) {
                servers[n] = {{"command", rs.mcp_exe}, {"args", proxy_args(rs, n)}, {"trust", true}, {"timeout", 3600000}};
                out.args.push_back("--allowed-mcp-server-names");
                out.args.push_back(n);
            }
            if (!write_file_atomic(P(rs.mcp_file), json{{"mcpServers", servers}}.dump(2))) { error = "Could not write the MCP settings of the run."; return false; }
            out.env.push_back({"GEMINI_CLI_SYSTEM_SETTINGS_PATH", rs.mcp_file});
        }
        if (!rs.model.empty()) { out.args.push_back("-m"); out.args.push_back(rs.model); }
        if (!rs.session_id.empty()) { out.args.push_back("--resume"); out.args.push_back(rs.session_id); }
        for (auto& d : rs.add_dirs) { out.args.push_back("--include-directories"); out.args.push_back(d); }
        if (!rs.system_file.empty()) out.env.push_back({"GEMINI_SYSTEM_MD", rs.system_file});
        return true;
    }
    if (rs.provider == "opencode") {
        out.exe = opencode_path();
        if (out.exe.empty()) {
            error = "OpenCode was not found on this PC (is `opencode` installed and on PATH?).";
            return false;
        }
        if (rs.mcp_file.empty() || rs.system_file.empty()) { error = "The run has no OpenCode settings file."; return false; }
        // Lean profile: one agent "phantom" from the run's own config file (our system prompt, a step limit). Everything is
        // denied first, then the skills' tools are allowed: denying single tools is not enough, "task" would hand the work
        // to a sub-agent with every tool.
        json perm = {{"*", "deny"}};
        for (auto& t : rs.tools) {
            if (t == "Read") { perm["read"] = "allow"; perm["list"] = "allow"; }
            else if (t == "Glob") perm["glob"] = "allow";
            else if (t == "Grep") perm["grep"] = "allow";
            else if (t == "Write" || t == "Edit" || t == "MultiEdit" || t == "NotebookEdit") perm["edit"] = "allow";
            else if (t == "Bash") perm["bash"] = "allow";
            else if (t == "WebFetch") perm["webfetch"] = "allow";
            else if (t == "WebSearch") perm["websearch"] = "allow";
        }
        if (!rs.add_dirs.empty()) {  // folders outside the work folder (the "*" rule above denies them)
            json ext = {{"*", "deny"}};
            for (auto& d : rs.add_dirs) {
                std::string dir = replace_all(d, "/", "\\");
                while (!dir.empty() && dir.back() == '\\') dir.pop_back();
                ext[dir] = "allow";
                ext[dir + "\\*"] = "allow";
            }
            perm["external_directory"] = ext;
        }
        // A local model server: OpenCode knows the model only when the config lists it (no key is needed).
        json provider = json::object();
        for (auto& l : opencode_locals()) {
            const std::string pre = std::string(l.id) + "/";
            if (rs.model.rfind(pre, 0) != 0 || rs.model.size() == pre.size()) continue;
            const std::string m = rs.model.substr(pre.size());
            provider[l.id] = {{"npm", "@ai-sdk/openai-compatible"}, {"name", l.label}, {"options", {{"baseURL", std::string(l.host) + "/v1"}}},
                              {"models", {{m, {{"name", m}, {"tool_call", true}}}}}};
        }
        json mcp = json::object();
        for (auto& n : rs.mcp) {
            json cmd = json::array({rs.mcp_exe});
            for (auto& a : proxy_args(rs, n)) cmd.push_back(a);
            mcp[n] = {{"type", "local"}, {"command", cmd}, {"enabled", true}, {"timeout", 3600000}};  // a call may wait for a person's approval
            perm[n + "_*"] = "allow";  // OpenCode names MCP tools "<server>_<tool>"
        }
        // {file:...} makes OpenCode read the prompt itself (it is never parsed as part of this JSON).
        json cfg = {{"$schema", "https://opencode.ai/config.json"}, {"autoupdate", false}, {"share", "disabled"}, {"mcp", mcp}, {"provider", provider},
                    {"agent", {{"phantom", {{"mode", "primary"}, {"prompt", "{file:" + replace_all(rs.system_file, "\\", "/") + "}"},
                                            {"steps", rs.max_turns}, {"permission", perm}}}}}};
        if (!write_file_atomic(P(rs.mcp_file), cfg.dump(2))) { error = "Could not write the OpenCode settings of the run."; return false; }
        // An empty config folder of our own: the user's global OpenCode config (agents, plugins, MCP servers) stays out.
        const fs::path cfg_home = P(rs.mcp_file).parent_path() / L"OpenCode_Config";
        std::error_code ec;
        fs::create_directories(cfg_home, ec);
        out.env.push_back({"OPENCODE_CONFIG", rs.mcp_file});
        out.env.push_back({"XDG_CONFIG_HOME", U8(cfg_home)});
        out.env.push_back({"OPENCODE_CONFIG_CONTENT", ""});
        out.env.push_back({"OPENCODE_CONFIG_DIR", ""});
        out.env.push_back({"OPENCODE_PERMISSION", ""});
        out.env.push_back({"OPENCODE_DISABLE_PROJECT_CONFIG", "1"});  // no opencode.json of the work folder
        out.env.push_back({"OPENCODE_DISABLE_CLAUDE_CODE", "1"});     // no CLAUDE.md, no ~/.claude/skills
        out.env.push_back({"OPENCODE_DISABLE_EXTERNAL_SKILLS", "1"});
        out.env.push_back({"OPENCODE_DISABLE_AUTOUPDATE", "1"});
        out.env.push_back({"OPENCODE_DISABLE_LSP_DOWNLOAD", "1"});
        out.stdin_data = rs.prompt;
        out.args = {"run", "--pure", "--format", "json", "--agent", "phantom", "--dir", rs.workdir};
        if (!rs.model.empty()) { out.args.push_back("-m"); out.args.push_back(rs.model); }
        if (!rs.session_id.empty()) { out.args.push_back("-s"); out.args.push_back(rs.session_id); }
        return true;
    }
    error = "Unknown provider: " + rs.provider;
    return false;
}

std::vector<std::string> collect_codex_images(const std::string& session_id, fs::file_time_type since, const fs::path& workdir, const fs::path& codex_home) {
    std::vector<std::string> out;
    if (session_id.empty() || session_id.find_first_not_of("abcdefABCDEF0123456789-") != std::string::npos) return out;
    const fs::path dir = codex_home_dir(codex_home) / L"generated_images" / P(session_id);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->last_write_time(ec) < since) continue;
        const fs::path dest = workdir / L"Generated_Images" / it->path().filename();
        if (fs::exists(dest, ec)) continue;
        fs::create_directories(dest.parent_path(), ec);
        if (fs::copy_file(it->path(), dest, ec)) out.push_back("Generated_Images/" + U8(it->path().filename()));
    }
    return out;
}

fs::path codex_home_dir(const fs::path& codex_home) {
    if (!codex_home.empty()) return codex_home;
    const std::string home = env(L"CODEX_HOME");
    return home.empty() ? P(env(L"USERPROFILE")) / L".codex" : P(home);
}

json codex_rate_limits(const std::string& session_id, const fs::path& codex_home) {
    if (session_id.empty() || session_id.find_first_not_of("abcdefABCDEF0123456789-") != std::string::npos) return nullptr;
    const fs::path sessions = codex_home_dir(codex_home) / L"sessions";
    const std::string tail = "-" + session_id + ".jsonl";
    std::error_code ec;
    std::time_t now = std::time(nullptr);
    for (int back = 0; back < 3; ++back) {  // the folders are named after the local day the session started
        std::time_t t = now - back * 86400;
        std::tm lt = *std::localtime(&t);
        char day[16];
        snprintf(day, sizeof day, "%04d/%02d/%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        const fs::path dir = sessions / P(day);
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = U8(e.path().filename());
            if (name.size() < tail.size() || name.compare(name.size() - tail.size(), tail.size(), tail) != 0) continue;
            std::string text;
            if (!read_file(e.path(), text)) return nullptr;
            json last = nullptr;
            size_t pos = 0;
            while (pos < text.size()) {  // the newest line with rate limits wins
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                if (text.find("\"rate_limits\"", pos) < end) {
                    json j = json::parse(text.substr(pos, end - pos), nullptr, false);
                    const json* p = nullptr;
                    if (j.is_object() && j.contains("payload") && j["payload"].is_object()) {
                        if (j["payload"].contains("rate_limits")) p = &j["payload"]["rate_limits"];
                        else if (j["payload"].contains("info") && j["payload"]["info"].is_object() && j["payload"]["info"].contains("rate_limits"))
                            p = &j["payload"]["info"]["rate_limits"];
                    }
                    if (p && p->is_object()) last = *p;
                }
                pos = end + 1;
            }
            return last;
        }
    }
    return nullptr;
}

std::string images_note(const std::vector<std::string>& images) {
    if (images.empty()) return "";
    std::string s = "\n\n(Images made in this run were saved in the work folder:";
    for (auto& i : images) s += " " + i;
    return s + ")";
}

// ------------------------------------------------------------------- parsing

static std::string tool_summary(const json& input) {
    if (!input.is_object()) return "";
    for (const char* k : {"command", "file_path", "filePath", "absolute_path", "path", "dir_path", "pattern", "url", "description", "query", "prompt"})
        if (input.contains(k) && input[k].is_string()) return truncate_utf8(input[k].get<std::string>(), 300);
    return truncate_utf8(dumps(input), 300);
}

static std::string content_text(const json& c) {
    if (c.is_string()) return c.get<std::string>();
    std::string out;
    if (c.is_array())
        for (auto& b : c)
            if (b.is_object() && b.value("type", "") == "text") out += b.value("text", "") + "\n";
    return out;
}

// Gemini wraps API errors in escaped JSON ([API Error: {\"error\":{\"message\":\"{\n  \\\"error\\\": ...): keep the innermost message.
static std::string clean_gemini_error(const std::string& m) {
    std::string flat;
    for (char c : m) if (c != '\\') flat += c;
    size_t at = flat.rfind("\"message\":");
    if (at != std::string::npos) {
        size_t q = flat.find('"', at + 10);
        size_t e = q == std::string::npos ? q : flat.find('"', q + 1);
        if (e != std::string::npos && e > q + 1) return truncate_utf8(flat.substr(q + 1, e - q - 1), 400);
    }
    return truncate_utf8(m, 400);
}

// 429: a free API key has small per-minute/per-day limits and none (limit: 0) for the Pro models.
std::string LineParser::gemini_error(const std::string& raw) const {
    if (raw.find("exceeded your current quota") == std::string::npos && raw.find("RESOURCE_EXHAUSTED") == std::string::npos &&
        raw.find("status 429") == std::string::npos)
        return clean_gemini_error(raw);
    return "Gemini says the quota" + (model_.empty() || model_ == "auto" ? std::string() : " of " + model_) +
           " is used up (429). A free API key has small per-minute and per-day limits and no quota for Pro models: wait a little and try again, "
           "write gemini-flash-lite-latest in the ghost's Model field, or use a key with billing.";
}

// gemini -o stream-json: init, message (assistant text comes in deltas), tool_use, tool_result, error, result
std::vector<json> LineParser::feed_gemini(const json& j, const std::string& type) {
    std::vector<json> ev;
    auto flush = [&] {  // the text since the last tool call becomes one message event
        if (!pending_.empty()) ev.push_back({{"kind", "message"}, {"text", pending_}});
    };
    if (type == "init") {
        if (!j.value("session_id", "").empty()) session_id = j.value("session_id", "");
        model_ = j.value("model", "");
        ev.push_back({{"kind", "status"}, {"text", "Session started"}, {"model", model_}});
    } else if (type == "message" && j.value("role", "") == "assistant") {
        pending_ += j.value("content", "");
    } else if (type == "tool_use") {
        flush();
        pending_.clear();
        ev.push_back({{"kind", "tool_use"}, {"name", j.value("tool_name", "tool")}, {"text", tool_summary(j.value("parameters", json::object()))}});
    } else if (type == "tool_result") {
        std::string out = j.contains("output") && j["output"].is_string() ? j["output"].get<std::string>() : "";
        if (out.empty() && j.contains("error") && j["error"].is_object()) out = j["error"].value("message", "");
        ev.push_back({{"kind", "tool_result"}, {"is_error", j.value("status", "") == "error"}, {"text", truncate_utf8(out, 1500)}});
    } else if (type == "error") {
        const std::string m = gemini_error(j.value("message", ""));
        ev.push_back({{"kind", j.value("severity", "error") == "warning" ? "status" : "error"}, {"text", m}});
    } else if (type == "result") {
        flush();
        if (j.contains("stats") && j["stats"].is_object()) {
            usage = j["stats"];
            tokens_in = usage.value("input_tokens", 0LL);
            tokens_out = usage.value("output_tokens", 0LL);
        }
        if (j.value("status", "") == "error") {
            is_error = true;
            std::string m = j.contains("error") && j["error"].is_object() ? j["error"].value("message", "") : "";
            if (m.find("turn") != std::string::npos && m.find("max") != std::string::npos) max_turns = true;
            result_text = m.empty() ? "The run failed." : gemini_error(m);
            ev.push_back({{"kind", "error"}, {"text", result_text}});
        } else {
            result_text = pending_;
        }
        pending_.clear();
    }
    return ev;
}

// An OpenCode error event: {name, data: {message, statusCode, responseBody}}. Keeps the service's own short reason.
static std::string opencode_error(const json& e) {
    const json d = e.is_object() && e.contains("data") && e["data"].is_object() ? e["data"] : json::object();
    std::string m = d.contains("message") && d["message"].is_string() ? d["message"].get<std::string>() : e.is_object() ? e.value("name", "") : "";
    const int code = d.contains("statusCode") && d["statusCode"].is_number() ? d["statusCode"].get<int>() : 0;
    std::string detail;
    const json body = json::parse(d.contains("responseBody") && d["responseBody"].is_string() ? d["responseBody"].get<std::string>() : "", nullptr, false);
    if (body.is_object()) {
        if (body.contains("detail") && body["detail"].is_string()) detail = body["detail"];
        else if (body.contains("error") && body["error"].is_object() && body["error"].contains("message") && body["error"]["message"].is_string()) detail = body["error"]["message"];
        else if (body.contains("message") && body["message"].is_string()) detail = body["message"];
    }
    if (code == 429)
        return "The model service says the quota is used up or the requests come too fast (429). Wait a little and try again, or write another model in the ghost's Model field.";
    if (code == 401 || code == 403)
        return "The model service refused the API key (" + std::to_string(code) + (detail.empty() ? "" : ": " + truncate_utf8(detail, 200)) + ").";
    if (e.is_object() && e.value("name", "") == "UnknownError")
        return truncate_utf8(m, 300) + " (Is the ghost's Model field right? OpenCode models look like service/model, e.g. nvidia/nvidia/nemotron-3.5-lightning-30b-a3b.)";
    if (code && !detail.empty()) return "The model service answered " + std::to_string(code) + ": " + truncate_utf8(detail, 300);
    return m.empty() ? "The run failed." : truncate_utf8(m, 400);
}

// opencode run --format json: step_start, text, reasoning, tool_use (one event when the call has finished), step_finish
// (tokens, cost), error. Every line carries the sessionID. There is no closing event: the answer is the text after the last tool call.
std::vector<json> LineParser::feed_opencode(const json& j, const std::string& type) {
    std::vector<json> ev;
    const std::string sid = j.value("sessionID", "");
    if (!sid.empty() && session_id.empty()) {
        session_id = sid;
        ev.push_back({{"kind", "status"}, {"text", "Session started"}});
    }
    const json part = j.contains("part") && j["part"].is_object() ? j["part"] : json::object();
    if (type == "text") {
        const std::string t = part.value("text", "");
        if (t.empty()) return ev;
        if (after_tool_) { pending_.clear(); after_tool_ = false; }
        pending_ += (pending_.empty() ? "" : "\n\n") + t;
        result_text = pending_;
        ev.push_back({{"kind", "message"}, {"text", t}});
    } else if (type == "reasoning") {
        const std::string t = part.value("text", "");
        if (!t.empty()) ev.push_back({{"kind", "thinking"}, {"text", truncate_utf8(t, 2000)}});
    } else if (type == "tool_use") {
        const json st = part.contains("state") && part["state"].is_object() ? part["state"] : json::object();
        ev.push_back({{"kind", "tool_use"}, {"name", part.value("tool", "tool")}, {"text", tool_summary(st.value("input", json::object()))}});
        std::string out = st.contains("output") && st["output"].is_string() ? st["output"].get<std::string>() : "";
        if (out.empty() && st.contains("error") && st["error"].is_string()) out = st["error"];
        ev.push_back({{"kind", "tool_result"}, {"is_error", st.value("status", "") == "error"}, {"text", truncate_utf8(out, 1500)}});
        after_tool_ = true;
    } else if (type == "step_finish") {
        if (part.contains("tokens") && part["tokens"].is_object()) {
            const json t = part["tokens"];
            const json c = t.contains("cache") && t["cache"].is_object() ? t["cache"] : json::object();
            tokens_in += t.value("input", 0LL) + c.value("read", 0LL) + c.value("write", 0LL);
            tokens_out += t.value("output", 0LL) + t.value("reasoning", 0LL);
            usage = {{"input_tokens", tokens_in}, {"output_tokens", tokens_out}};
        }
        if (part.contains("cost") && part["cost"].is_number()) cost_usd = (cost_usd < 0 ? 0 : cost_usd) + part["cost"].get<double>();
    } else if (type == "error") {
        is_error = true;
        result_text = opencode_error(j.value("error", json::object()));
        ev.push_back({{"kind", "error"}, {"text", result_text}});
    }
    return ev;
}

std::vector<json> LineParser::feed(const std::string& line) {
    std::vector<json> ev;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        // Gemini without a sign-in prints plain text and stops: "Please set an Auth method in your ...settings.json or ..."
        if (provider_ == "gemini" && result_text.empty()) {
            if (line.find("Please set an Auth method") != std::string::npos) {
                is_error = true;
                result_text = "Gemini CLI is not signed in on this PC.";
            } else if (line.find("IneligibleTierError") != std::string::npos || line.find("no longer supported for Gemini Code Assist") != std::string::npos) {
                // Google refuses the free Google sign-in for Gemini CLI ("migrate to Antigravity"); the CLI would only print more of it.
                is_error = stop_now = true;
                result_text = "Google no longer lets Gemini CLI use a free Google sign-in. Add a Gemini API key in Settings > Secrets and pick it as this ghost's brain.";
            } else if (line.rfind("Attempt ", 0) == 0 && line.find("failed with status 429") != std::string::npos && ++quota_hits_ >= 3) {
                // The CLI retries a used-up quota about ten times with growing pauses: the run looks stuck for minutes. Stop at the 3rd.
                is_error = stop_now = true;
                result_text = gemini_error(line);
            }
        }
        ev.push_back({{"kind", "log"}, {"text", truncate_utf8(line, 2000)}});
        return ev;
    }
    const std::string type = j.value("type", "");

    if (provider_ == "claude") {
        if (type == "system" && j.value("subtype", "") == "init") {
            if (!j.value("session_id", "").empty()) session_id = j.value("session_id", "");
            ev.push_back({{"kind", "status"}, {"text", "Session started"}, {"model", j.value("model", "")}});
        } else if (type == "assistant" && j.contains("message") && j["message"].contains("content") &&
                   j["message"]["content"].is_array()) {
            for (auto& b : j["message"]["content"]) {
                std::string bt = b.value("type", "");
                if (bt == "text" && !b.value("text", "").empty())
                    ev.push_back({{"kind", "message"}, {"text", b["text"]}});
                else if (bt == "thinking" && !b.value("thinking", "").empty())
                    ev.push_back({{"kind", "thinking"}, {"text", truncate_utf8(b["thinking"], 2000)}});
                else if (bt == "tool_use")
                    ev.push_back({{"kind", "tool_use"}, {"name", b.value("name", "tool")},
                                  {"text", tool_summary(b.value("input", json::object()))}});
            }
        } else if (type == "user" && j.contains("message") && j["message"].contains("content") &&
                   j["message"]["content"].is_array()) {
            for (auto& b : j["message"]["content"])
                if (b.value("type", "") == "tool_result")
                    ev.push_back({{"kind", "tool_result"}, {"is_error", b.value("is_error", false)},
                                  {"text", truncate_utf8(content_text(b.value("content", json())), 1500)}});
        } else if (type == "result") {
            result_text = j.value("result", "");
            is_error = j.value("is_error", false);
            if (j.value("subtype", "") == "error_max_turns") max_turns = true;
            if (!j.value("session_id", "").empty()) session_id = j.value("session_id", "");
            if (j.contains("total_cost_usd") && j["total_cost_usd"].is_number()) cost_usd = j["total_cost_usd"];
            if (j.contains("usage")) {
                usage = j["usage"];
                tokens_in = usage.value("input_tokens", 0LL) + usage.value("cache_creation_input_tokens", 0LL) + usage.value("cache_read_input_tokens", 0LL);
                tokens_out = usage.value("output_tokens", 0LL);
            }
            if (is_error) ev.push_back({{"kind", "error"}, {"text", result_text.empty() ? "The run reported an error." : result_text}});
        } else if (type == "rate_limit_event") {
            if (j.contains("rate_limit_info") && j["rate_limit_info"].is_object()) rate_limits = j["rate_limit_info"];
            std::string st = j.value("rate_limit_info", json::object()).value("status", "allowed");
            if (st != "allowed") ev.push_back({{"kind", "status"}, {"text", "Usage limit status: " + st}});
        }
        return ev;
    }

    if (provider_ == "gemini") return feed_gemini(j, type);
    if (provider_ == "opencode") return feed_opencode(j, type);

    // codex --json
    if (type == "thread.started") {
        session_id = j.value("thread_id", "");
        ev.push_back({{"kind", "status"}, {"text", "Session started"}});
    } else if (type == "item.started" || type == "item.completed") {
        const json item = j.value("item", json::object());
        const std::string it = item.value("type", "");
        const bool done = type == "item.completed";
        if (it == "agent_message" && done) {
            result_text = item.value("text", "");
            ev.push_back({{"kind", "message"}, {"text", result_text}});
        } else if (it == "reasoning" && done) {
            ev.push_back({{"kind", "thinking"}, {"text", truncate_utf8(item.value("text", ""), 2000)}});
        } else if (it == "command_execution") {
            if (!done) ev.push_back({{"kind", "tool_use"}, {"name", "shell"}, {"text", truncate_utf8(item.value("command", ""), 300)}});
            else ev.push_back({{"kind", "tool_result"}, {"is_error", item.value("exit_code", 0) != 0},
                               {"text", truncate_utf8(item.value("aggregated_output", ""), 1500)}});
        } else if (it == "file_change" && done) {
            std::string files;
            for (auto& c : item.value("changes", json::array())) files += c.value("kind", "change") + " " + c.value("path", "") + "\n";
            ev.push_back({{"kind", "tool_use"}, {"name", "file_change"}, {"text", truncate_utf8(files, 600)}});
        } else if (it == "error" && done) {
            ev.push_back({{"kind", "error"}, {"text", item.value("message", "error")}});
        } else if (done) {
            ev.push_back({{"kind", "log"}, {"text", truncate_utf8(dumps(item), 600)}});
        }
    } else if (type == "turn.completed") {
        if (j.contains("usage")) {
            usage = j["usage"];
            tokens_in += usage.value("input_tokens", 0LL);
            tokens_out += usage.value("output_tokens", 0LL);
        }
    } else if (type == "turn.failed" || type == "error") {
        std::string m = j.contains("error") && j["error"].is_object() ? j["error"].value("message", "") : j.value("message", "");
        if (type == "error" && m.rfind("Reconnecting", 0) == 0) {
            // "Reconnecting... 2/5 (...)": Codex retries by itself; only turn.failed or a final error ends the run
            ev.push_back({{"kind", "status"}, {"text", truncate_utf8(m, 200)}});
            return ev;
        }
        is_error = true;
        // The failure becomes the run's error text (e.g. "unexpected status 401 Unauthorized: Incorrect API key provided...").
        size_t cut = m.find(", url: ");
        if (result_text.empty() && !m.empty()) result_text = cut == std::string::npos ? m : m.substr(0, cut);
        ev.push_back({{"kind", "error"}, {"text", m.empty() ? "The run failed." : m}});
    }
    return ev;
}

}  // namespace pc2
