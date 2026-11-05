// `phantom_co.exe --mcp-proxy <name> --mcp-port <port> --mcp-key <key>`: one MCP server of one run.
// stdin/stdout are the MCP connection to the CLI (newline-delimited JSON-RPC). The proxy asks the running program for
// the server (command, secrets), then passes messages through to the real server or serves a built-in one. A tool call
// that needs approval waits until a person approves it in the chat; a declined call gets an error result instead.
#include <windows.h>

#include <mutex>
#include <set>
#include <thread>

#include "http_client.hpp"
#include "mcp.hpp"
#include "mcp_builtin.hpp"
#include "process.hpp"
#include "version.hpp"

namespace pc2 {

namespace {

HANDLE g_in = nullptr, g_out = nullptr, g_err = nullptr;
std::mutex g_out_mu;
int g_port = 0;
std::string g_key, g_name;
json g_session;

void write_out(const std::string& line) {
    std::lock_guard<std::mutex> l(g_out_mu);
    std::string s = line + "\n";
    DWORD n = 0;
    for (size_t at = 0; at < s.size(); at += n)
        if (!WriteFile(g_out, s.data() + at, (DWORD)(s.size() - at), &n, nullptr) || n == 0) return;
}

void log_err(const std::string& msg) {
    if (!g_err || g_err == INVALID_HANDLE_VALUE) return;
    std::string s = "[phantom mcp " + g_name + "] " + msg + "\n";
    DWORD n = 0;
    WriteFile(g_err, s.data(), (DWORD)s.size(), &n, nullptr);
}

// Reads lines from a handle until EOF; returns false at the end.
struct LineReader {
    explicit LineReader(HANDLE handle) : h(handle) {}
    HANDLE h;
    std::string buf;
    bool next(std::string& line) {
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                line = buf.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[65536];
            DWORD got = 0;
            if (!ReadFile(h, tmp, sizeof(tmp), &got, nullptr) || got == 0) {
                if (buf.empty()) return false;
                line.swap(buf);
                buf.clear();
                return true;
            }
            buf.append(tmp, got);
        }
    }
};

json host_post(const std::string& path, const json& body, int receive_ms = 40000) {
    HttpClient c("http://127.0.0.1:" + std::to_string(g_port), 5000, receive_ms);
    auto r = c.Post(path, {{"X-PC2", "1"}}, body.dump(), "application/json");
    if (!r) return json();
    json j = json::parse(r->body, nullptr, false);
    if (r->status != 200) return {{"error", j.is_object() ? j.value("error", "error") : "error " + std::to_string(r->status)}};
    return j;
}

bool needs_approval(const std::string& tool) {
    if (!g_session.value("approve", false)) return false;
    const json& list = g_session.value("approve_tools", json::array());
    if (list.empty()) return true;
    for (auto& t : list) if (t == tool) return true;
    return false;
}

// Waits for a person's answer. "" = approved, else the reason the call must not run.
std::string wait_for_approval(const std::string& tool, const json& args) {
    std::string id;
    int failures = 0;
    for (;;) {
        json r = host_post("/api/mcp/ask", {{"key", g_key}, {"server", g_name}, {"tool", tool}, {"arguments", args}, {"id", id}}, 45000);
        if (r.is_null() || r.contains("error")) {
            if (++failures >= 3) return "Phantom Co. could not ask for approval, so the action was not done.";
            Sleep(2000);
            continue;
        }
        failures = 0;
        id = r.value("id", id);
        const std::string st = r.value("status", "");
        if (st == "approved") return "";
        if (st == "declined") return r.value("by", "The person") + " declined this action. Do not try it again; tell the person what you wanted to do and ask how to continue.";
        if (st != "pending") return "Nobody approved this action before the work ended, so it was not done.";
    }
}

json error_result(const json& id, const std::string& text) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", true}}}};
}

// ---------------------------------------------------------------- built-in
int serve_builtin() {
    const std::string kind = g_session.value("builtin", "");
    LineReader in{g_in};
    std::string line;
    std::vector<std::thread> calls;
    while (in.next(line)) {
        if (line.empty()) continue;
        json m = json::parse(line, nullptr, false);
        if (!m.is_object() || !m.contains("method")) continue;  // responses / garbage: a built-in server sends no requests
        const std::string method = m.value("method", "");
        if (!m.contains("id")) continue;  // notifications (initialized, cancelled...)
        const json id = m["id"];
        if (method == "initialize") {
            std::string ver = m.contains("params") ? m["params"].value("protocolVersion", "2025-06-18") : "2025-06-18";
            write_out(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", ver}, {"capabilities", {{"tools", json::object()}}},
                                                                        {"serverInfo", {{"name", "phantom-" + kind}, {"version", PC2_VERSION_STR}}}}}}.dump());
        } else if (method == "tools/list") {
            write_out(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", builtin::tools(kind)}}}}.dump());
        } else if (method == "ping") {
            write_out(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}}.dump());
        } else if (method == "tools/call") {
            const json params = m.value("params", json::object());
            calls.emplace_back([id, params, kind] {  // uploads take a while: other messages go on meanwhile
                const std::string tool = params.value("name", "");
                const json args = params.value("arguments", json::object());
                if (needs_approval(tool)) {
                    std::string why = wait_for_approval(tool, args);
                    if (!why.empty()) { write_out(error_result(id, why).dump()); return; }
                }
                if (kind == "telegram" && tool == "read_messages" && g_session.value("options", json::object()).value("listen", false)) {
                    // Phantom Co. itself reads the bot (only one reader is possible): ask it for what came in.
                    json r = host_post("/api/mcp/telegram_inbox", {{"key", g_key}, {"server", g_name}});
                    std::string out;
                    int limit = args.contains("limit") && args["limit"].is_number_integer() ? std::clamp(args["limit"].get<int>(), 1, 50) : 20;
                    const json msgs = r.is_object() ? r.value("messages", json::array()) : json::array();
                    for (size_t i = msgs.size() > (size_t)limit ? msgs.size() - limit : 0; i < msgs.size(); ++i) {
                        const json& m = msgs[i];
                        std::time_t t = (std::time_t)m.value("date", 0LL);
                        char when[32] = "";
                        std::tm tmv{};
                        if (localtime_s(&tmv, &t) == 0) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
                        out += std::string(when) + " · chat " + std::to_string(m.value("chat", 0LL)) + " (" + m.value("chat_name", "") + ") · " +
                               m.value("from", "") + ": " + m.value("text", "") + "\n";
                    }
                    if (out.empty()) out = "No messages since Phantom Co. started.";
                    json content = json::array();
                    content.push_back({{"type", "text"}, {"text", out}});
                    json msg = {{"jsonrpc", "2.0"}, {"id", id}};
                    msg["result"] = {{"content", content}};
                    write_out(msg.dump());
                    return;
                }
                write_out(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", builtin::call(kind, tool, args, g_session)}}.dump());
            });
        } else {
            write_out(json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Method not found"}}}}.dump());
        }
    }
    for (auto& t : calls) if (t.joinable()) t.join();
    return 0;
}

// ---------------------------------------------------------------- command
std::string find_command(const std::string& cmd) {
    std::wstring w = Widen(cmd);
    if (w.find_first_of(L"\\/") != std::wstring::npos) return cmd;
    for (const wchar_t* ext : {L".exe", L".cmd", L".bat", L""}) {
        wchar_t buf[MAX_PATH * 2];
        DWORD r = SearchPathW(nullptr, w.c_str(), ext, MAX_PATH * 2, buf, nullptr);
        if (r > 0 && r < MAX_PATH * 2) return Narrow(std::wstring(buf, r));
    }
    return "";
}

int serve_command() {
    const std::string exe = find_command(g_session.value("command", ""));
    if (exe.empty()) { log_err("command not found: " + g_session.value("command", "")); return 1; }
    std::vector<std::string> args;
    for (auto& a : g_session.value("args", json::array())) args.push_back(a.get<std::string>());
    std::vector<std::pair<std::string, std::string>> env;
    const json envs = g_session.value("env", json::object());  // a named copy: items() of a temporary would dangle
    for (auto& [k, v] : envs.items()) if (v.is_string() && !v.get<std::string>().empty()) env.push_back({k, v.get<std::string>()});

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE c_in_r = nullptr, c_in_w = nullptr, c_out_r = nullptr, c_out_w = nullptr;
    if (!CreatePipe(&c_in_r, &c_in_w, &sa, 1 << 20) || !CreatePipe(&c_out_r, &c_out_w, &sa, 1 << 20)) return 1;
    SetHandleInformation(c_in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c_out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE err = g_err;
    HANDLE nul = nullptr;
    if (!err || err == INVALID_HANDLE_VALUE) err = nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    else SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = c_in_r;
    si.hStdOutput = c_out_w;
    si.hStdError = err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line(exe, args), block = environment_block(env), cwd = Widen(g_session.value("cwd", ""));
    std::error_code ec;
    if (!cwd.empty()) fs::create_directories(cwd, ec);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             block.empty() ? nullptr : block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(c_in_r);
    CloseHandle(c_out_w);
    if (nul) CloseHandle(nul);
    if (!ok) { log_err("could not start " + exe + " (error " + std::to_string(GetLastError()) + ")"); return 1; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::mutex child_mu;
    auto to_child = [&](const std::string& line) {
        std::lock_guard<std::mutex> l(child_mu);
        std::string s = line + "\n";
        DWORD n = 0;
        for (size_t at = 0; at < s.size(); at += n)
            if (!WriteFile(c_in_w, s.data() + at, (DWORD)(s.size() - at), &n, nullptr) || n == 0) return;
    };
    std::thread from_child([&] {  // the server's answers go straight to the CLI
        LineReader r{c_out_r};
        std::string line;
        while (r.next(line)) write_out(line);
        ExitProcess(0);  // the server ended: so does the proxy (the CLI sees the connection close)
    });
    from_child.detach();

    LineReader in{g_in};
    std::string line;
    while (in.next(line)) {
        json m = json::parse(line, nullptr, false);
        if (m.is_object() && m.value("method", "") == "tools/call" && m.contains("id")) {
            const json params = m.value("params", json::object());
            const std::string tool = params.value("name", "");
            if (needs_approval(tool)) {
                std::thread([&to_child, line, m, params, tool] {
                    std::string why = wait_for_approval(tool, params.value("arguments", json::object()));
                    if (why.empty()) to_child(line);
                    else write_out(error_result(m["id"], why).dump());
                }).detach();
                continue;
            }
        }
        to_child(line);
    }
    // the CLI closed the connection: give the server a moment to end, then end it
    { std::lock_guard<std::mutex> l(child_mu); CloseHandle(c_in_w); c_in_w = nullptr; }
    WaitForSingleObject(pi.hProcess, 3000);
    if (job) CloseHandle(job);  // kills what is left
    CloseHandle(pi.hProcess);
    return 0;
}

}  // namespace

int mcp_proxy_main(const std::string& name, int port, const std::string& key) {
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = GetStdHandle(STD_ERROR_HANDLE);
    g_name = name;
    g_port = port;
    g_key = key;
    if (!g_in || g_in == INVALID_HANDLE_VALUE || !g_out || g_out == INVALID_HANDLE_VALUE) return 1;
    json s = host_post("/api/mcp/session", {{"key", key}, {"server", name}});
    if (s.is_null() || s.contains("error")) {
        log_err(s.is_null() ? "Phantom Co. could not be reached" : s.value("error", "refused"));
        return 1;
    }
    g_session = s;
    return g_session.value("kind", "") == "builtin" ? serve_builtin() : serve_command();
}

}  // namespace pc2
