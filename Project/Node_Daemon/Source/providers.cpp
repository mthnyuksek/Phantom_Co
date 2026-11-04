#include "providers.hpp"

#include <windows.h>

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

std::vector<ProviderInfo> detect_providers() {
    std::string c = claude_path(), x = codex_path();
    return {{"claude", "Claude Code", !c.empty(), c},
            {"codex", "Codex", !x.empty(), x},
            {"demo", "Demo (simulated, no subscription used)", true, ""}};
}

std::string provider_exe(const std::string& id) { return id == "claude" ? claude_path() : id == "codex" ? codex_path() : ""; }

bool provider_known(const std::string& id) { return id == "claude" || id == "codex" || id == "demo"; }

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
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
        // Lean profile: own system prompt, only the tools the skills grant, no MCP servers, no slash commands.
        out.args = {"-p", "--output-format", "stream-json", "--verbose", "--system-prompt-file", rs.system_file,
                    "--tools", join(rs.tools), "--strict-mcp-config", "--disable-slash-commands",
                    "--setting-sources", "project", "--permission-prompts", "none", "--max-turns", std::to_string(rs.max_turns)};
        if (!rs.tools.empty()) { out.args.push_back("--allowedTools"); out.args.push_back(join(rs.tools)); }
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
        out.args.push_back("-");  // read the prompt from stdin
        return true;
    }
    error = "Unknown provider: " + rs.provider;
    return false;
}

std::vector<std::string> collect_codex_images(const std::string& session_id, fs::file_time_type since, const fs::path& workdir, const fs::path& codex_home) {
    std::vector<std::string> out;
    if (session_id.empty() || session_id.find_first_not_of("abcdefABCDEF0123456789-") != std::string::npos) return out;
    const std::string home = env(L"CODEX_HOME");
    const fs::path base = !codex_home.empty() ? codex_home : home.empty() ? P(env(L"USERPROFILE")) / L".codex" : P(home);
    const fs::path dir = base / L"generated_images" / P(session_id);
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

std::string images_note(const std::vector<std::string>& images) {
    if (images.empty()) return "";
    std::string s = "\n\n(Images made in this run were saved in the work folder:";
    for (auto& i : images) s += " " + i;
    return s + ")";
}

// ------------------------------------------------------------------- parsing

static std::string tool_summary(const json& input) {
    if (!input.is_object()) return "";
    for (const char* k : {"command", "file_path", "path", "pattern", "url", "description", "query"})
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

std::vector<json> LineParser::feed(const std::string& line) {
    std::vector<json> ev;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
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
            std::string st = j.value("rate_limit_info", json::object()).value("status", "allowed");
            if (st != "allowed") ev.push_back({{"kind", "status"}, {"text", "Usage limit status: " + st}});
        }
        return ev;
    }

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
