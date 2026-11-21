#include "assistant_mcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

namespace pc2 {

namespace {

constexpr int kDefaultWait = 45;  // many MCP clients give up on a tool call after 60 s
constexpr int kMaxWait = 300;
constexpr size_t kMaxMessage = 12000;  // bytes of one message in a tool answer
constexpr size_t kMaxAnswer = 60000;   // bytes of one tool answer

struct ToolError {
    std::string text;
};

// ------------------------------------------------------------- arguments
std::string arg_str(const json& a, const char* k, bool required = false, size_t max = 20000) {
    if (a.contains(k) && a[k].is_string()) {
        std::string s = a[k].get<std::string>();
        if (!s.empty() || !required) return truncate_utf8(s, max);
    }
    if (a.contains(k) && a[k].is_number()) return std::to_string(a[k].get<long long>());
    if (required) throw ToolError{std::string("Missing argument: ") + k};
    return "";
}
std::string arg_id(const json& a, const char* k, bool required = true) {
    std::string s = arg_str(a, k, required, 64);
    if (!s.empty() && !valid_id(s)) throw ToolError{std::string("Not a valid id: ") + k + " = " + s};
    return s;
}
int arg_int(const json& a, const char* k, int def, int lo, int hi) {
    int v = def;
    if (a.contains(k) && a[k].is_number()) v = (int)a[k].get<double>();
    else if (a.contains(k) && a[k].is_string()) { try { v = std::stoi(a[k].get<std::string>()); } catch (...) {} }
    return std::clamp(v, lo, hi);
}
bool arg_bool(const json& a, const char* k, bool def) {
    if (a.contains(k) && a[k].is_boolean()) return a[k].get<bool>();
    return def;
}

// ------------------------------------------------------------- API calls
json api_ok(const AssistantApi& api, const std::string& method, const std::string& path, const json& body = nullptr) {
    ApiResult r = api.call(method, path, body);
    if (r.status >= 200 && r.status < 300) return r.body;
    std::string err = r.body.is_object() ? r.body.value("error", "") : "";
    if (err.empty()) err = r.status ? "Phantom Co. answered " + std::to_string(r.status) : "Phantom Co. did not answer.";
    if (r.body.is_object() && r.body.contains("check") && r.body["check"].contains("errors"))
        for (const json& e : r.body["check"]["errors"]) err += "\n- " + (e.is_string() ? e.get<std::string>() : e.dump());
    throw ToolError{err};
}

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Names {
    std::map<std::string, std::string> agents, users;
};
Names names_of(const json& state) {
    Names n;
    for (const json& a : state.value("agents", json::array())) n.agents[a.value("id", "")] = a.value("name", "");
    for (const json& u : state.value("users", json::array())) n.users[u.value("id", "")] = u.value("name", "");
    return n;
}
std::string who(const Names& n, const std::string& from) {
    if (from.rfind("agent:", 0) == 0) {
        auto it = n.agents.find(from.substr(6));
        return (it != n.agents.end() && !it->second.empty() ? it->second : from.substr(6)) + " (ghost)";
    }
    if (from.rfind("user:", 0) == 0) {
        auto it = n.users.find(from.substr(5));
        return it != n.users.end() && !it->second.empty() ? it->second : from.substr(5);
    }
    return "System";
}

// A ghost given by id or by name (any case).
json find_ghost(const json& state, const std::string& g) {
    for (const json& a : state.value("agents", json::array()))
        if (a.value("id", "") == g) return a;
    for (const json& a : state.value("agents", json::array()))
        if (lower(a.value("name", "")) == lower(g)) return a;
    throw ToolError{"No ghost called \"" + g + "\". Call list_ghosts to see the ghosts."};
}

std::string message_line(const Names& n, const json& m) {
    std::string text = m.value("text", "");
    if (text.empty()) return "";
    return "[" + m.value("ts", "") + "] " + who(n, m.value("from", "")) + ":\n" + truncate_utf8(text, kMaxMessage) +
           (text.size() > kMaxMessage ? "\n…(cut)" : "") + "\n\n";
}

// Waits until no ghost works in the conversation (or the time is up). True when it is quiet.
bool wait_quiet(const AssistantApi& api, const std::string& conv, int secs) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    while (api.busy(conv)) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return true;
}

// What happened in a conversation since the last message of a person: the reply of an ask.
std::string reply_text(const AssistantApi& api, const std::string& conv, int wait) {
    bool quiet = wait_quiet(api, conv, wait);
    json v = api_ok(api, "GET", "/api/chats/" + conv);
    Names n = names_of(api_ok(api, "GET", "/api/state"));
    const json& msgs = v["messages"];
    int start = 0;
    for (int i = (int)msgs.size() - 1; i >= 0; --i)
        if (msgs[i].value("from", "").rfind("user:", 0) == 0) { start = i + 1; break; }
    std::string out = "chat_id: " + conv + "\nstatus: " + (quiet && !v.value("busy", false) ? "done" : "still working") + "\n";
    for (const json& ap : v.value("approvals", json::array()))
        out += "waiting for approval: " + ap.value("server", "") + " / " + ap.value("tool", "") +
               " (a person must answer it in Phantom Co. or Telegram; you cannot)\n";
    out += "\n";
    std::string body;
    for (int i = start; i < (int)msgs.size(); ++i) body += message_line(n, msgs[i]);
    if (body.empty()) body = quiet ? "(no answer was written)\n" : "(no answer yet: call get_reply with this chat_id later)\n";
    else if (!quiet) body += "(the ghost is still working: call get_reply with this chat_id for the rest)\n";
    return out + body;
}

std::string wake_text(const json& t) {
    std::string w = t.value("wake", "");
    if (w.empty()) return "";
    std::string s = w == "interval" ? "every " + std::to_string(t.value("wake_every", 0)) + " min" : "daily at " + t.value("wake_at", "");
    return s + (t.value("wake_on", false) ? "" : " (paused)");
}

json wake_body(const json& a) {
    std::string kind = arg_str(a, "wake", false, 16);
    json b = {{"wake", kind.empty() ? "none" : kind}};
    if (a.contains("every_min")) b["every_min"] = arg_int(a, "every_min", 0, 0, 10080);
    if (a.contains("at")) b["at"] = arg_str(a, "at", false, 8);
    return b;
}

std::string pretty(const json& j) { return truncate_utf8(dumps(j, 1), kMaxAnswer); }

// ------------------------------------------------------------- tools
struct Tool {
    const char* name;
    const char* description;
    json schema;
    std::function<std::string(const json&, const AssistantApi&)> run;
};

json obj(json props, json required = json::array()) {
    return {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}
json s_str(const char* d) { return {{"type", "string"}, {"description", d}}; }
json s_int(const char* d) { return {{"type", "integer"}, {"description", d}}; }
json s_bool(const char* d) { return {{"type", "boolean"}, {"description", d}}; }
const char* kWaitHelp = "Seconds to wait for the answer (default 45, max 300). Keep it below your MCP client's tool timeout.";
const char* kWakeHelp = "none | interval | daily";

const std::vector<Tool>& tools() {
    static const std::vector<Tool> list = {
        {"list_ghosts", "List the ghosts (AI agents) of the company: id, name, job, brain and skills.", obj(json::object()),
         [](const json&, const AssistantApi& api) {
             json st = api_ok(api, "GET", "/api/state");
             std::string out;
             for (const json& a : st.value("agents", json::array())) {
                 std::string skills;
                 for (const json& s : a.value("skills", json::array())) skills += (skills.empty() ? "" : ", ") + s.get<std::string>();
                 std::string brain = a.value("provider", "");
                 if (!a.value("model", "").empty()) brain += " / " + a.value("model", "");
                 out += "- " + a.value("name", "") + " (id: " + a.value("id", "") + ")" + (a.value("enabled", true) ? "" : " [disabled]") +
                        "\n  job: " + a.value("role", "") + "\n  brain: " + brain + "\n  skills: " + skills + "\n";
             }
             return out.empty() ? std::string("No ghosts yet. A person hires them in Phantom Co.") : out;
         }},
        {"list_projects", "List the projects (shared work folders with notes). Pass a project_id to ask_ghost or create_task to work inside one.",
         obj(json::object()),
         [](const json&, const AssistantApi& api) {
             json st = api_ok(api, "GET", "/api/state");
             std::string out;
             for (const json& p : st.value("projects", json::array()))
                 out += "- " + p.value("name", "") + " (id: " + p.value("id", "") + ")" +
                        (p.value("summary", "").empty() ? "" : "\n  " + truncate_utf8(p.value("summary", ""), 400)) + "\n";
             return out.empty() ? std::string("No projects yet.") : out;
         }},
        {"ask_ghost",
         "Send a message to a ghost and wait for its answer. Without chat_id a new chat is started; pass the chat_id from an earlier answer "
         "to go on in the same chat (the ghost remembers it). If the answer says 'still working', call get_reply later.",
         obj({{"ghost", s_str("Ghost id or name (for a new chat)")}, {"message", s_str("What to say")},
              {"chat_id", s_str("Go on in this chat instead of starting a new one")},
              {"project_id", s_str("Work inside this project (new chats only)")}, {"wait_seconds", s_int(kWaitHelp)}},
             {"message"}),
         [](const json& a, const AssistantApi& api) {
             std::string chat = arg_id(a, "chat_id", false), msg = arg_str(a, "message", true);
             if (chat.empty()) {
                 std::string g = arg_str(a, "ghost", false, 200);
                 if (g.empty()) throw ToolError{"Give a ghost (or a chat_id to go on in a chat)."};
                 json ghost = find_ghost(api_ok(api, "GET", "/api/state"), g);
                 json c = api_ok(api, "POST", "/api/chats", {{"agent_id", ghost["id"]}, {"project_id", arg_id(a, "project_id", false)}});
                 chat = c["meta"].value("id", "");
             }
             api_ok(api, "POST", "/api/chats/" + chat + "/send", {{"text", msg}});
             return reply_text(api, chat, arg_int(a, "wait_seconds", kDefaultWait, 0, kMaxWait));
         }},
        {"get_reply", "Wait for (or read) the answer in a chat or task chat after your last message.",
         obj({{"chat_id", s_str("The chat")}, {"wait_seconds", s_int(kWaitHelp)}}, {"chat_id"}),
         [](const json& a, const AssistantApi& api) {
             return reply_text(api, arg_id(a, "chat_id"), arg_int(a, "wait_seconds", kDefaultWait, 0, kMaxWait));
         }},
        {"list_chats", "List the chats you can see (newest first, at most 50).", obj(json::object()),
         [](const json&, const AssistantApi& api) {
             json list = api_ok(api, "GET", "/api/chats").value("chats", json::array());
             Names n = names_of(api_ok(api, "GET", "/api/state"));
             std::sort(list.begin(), list.end(), [](const json& x, const json& y) { return x.value("updated", "") > y.value("updated", ""); });
             std::string out;
             int k = 0;
             for (const json& c : list) {
                 if (++k > 50) break;
                 out += "- " + c.value("title", "") + " (chat_id: " + c.value("id", "") + ") with " + who(n, "agent:" + c.value("agent_id", "")) +
                        (c.value("busy", false) ? " [working]" : "") + (c.value("updated", "").empty() ? "" : ", " + c.value("updated", "")) + "\n";
             }
             return out.empty() ? std::string("No chats yet.") : out;
         }},
        {"read_chat", "Read the last messages of a chat or task chat.",
         obj({{"chat_id", s_str("The chat")}, {"last", s_int("How many messages (default 20, max 200)")}}, {"chat_id"}),
         [](const json& a, const AssistantApi& api) {
             std::string chat = arg_id(a, "chat_id");
             json v = api_ok(api, "GET", "/api/chats/" + chat);
             Names n = names_of(api_ok(api, "GET", "/api/state"));
             const json& msgs = v["messages"];
             int last = arg_int(a, "last", 20, 1, 200), start = std::max(0, (int)msgs.size() - last);
             std::string out = "chat: " + v["meta"].value("title", "") + " (chat_id: " + chat + ")" + (v.value("busy", false) ? " [working]" : "") + "\n\n";
             for (int i = start; i < (int)msgs.size(); ++i) out += message_line(n, msgs[i]);
             return truncate_utf8(out, kMaxAnswer);
         }},
        {"stop_work", "Stop the ghosts working in a chat or task chat.", obj({{"chat_id", s_str("The chat")}}, {"chat_id"}),
         [](const json& a, const AssistantApi& api) {
             api_ok(api, "POST", "/api/chats/" + arg_id(a, "chat_id") + "/stop", json::object());
             return std::string("Stopped.");
         }},
        {"list_tasks", "List the tasks (team-wide work items given to a ghost): status, wake-up and their chat_id.", obj(json::object()),
         [](const json&, const AssistantApi& api) {
             json list = api_ok(api, "GET", "/api/tasks").value("tasks", json::array());
             Names n = names_of(api_ok(api, "GET", "/api/state"));
             std::string out;
             for (const json& t : list) {
                 std::string w = wake_text(t);
                 out += "- " + t.value("title", "") + " (task_id: " + t.value("id", "") + ", chat_id: " + t.value("conversation_id", "") + ")\n  ghost: " +
                        who(n, "agent:" + t.value("agent_id", "")) + ", status: " + t.value("status", "") + (w.empty() ? "" : ", wakes " + w) + "\n";
             }
             return out.empty() ? std::string("No tasks yet.") : out;
         }},
        {"create_task",
         "Create a task for a ghost (visible to the whole team) and start it. Optional wake-up makes it run again by itself while Phantom Co. is open: "
         "wake=interval with every_min, or wake=daily with at (HH:MM, this PC's time).",
         obj({{"title", s_str("Short title")}, {"description", s_str("What the ghost should do")}, {"ghost", s_str("Ghost id or name")},
              {"project_id", s_str("Optional project")}, {"run", s_bool("Start it now (default true)")}, {"wake", s_str(kWakeHelp)},
              {"every_min", s_int("Minutes between runs (wake=interval, 1-10080)")}, {"at", s_str("HH:MM (wake=daily)")},
              {"wait_seconds", s_int("Seconds to wait for the first answer (default 0 = do not wait)")}},
             {"title", "ghost"}),
         [](const json& a, const AssistantApi& api) {
             json ghost = find_ghost(api_ok(api, "GET", "/api/state"), arg_str(a, "ghost", true, 200));
             json b = {{"title", arg_str(a, "title", true, 120)}, {"description", arg_str(a, "description")}, {"agent_id", ghost["id"]},
                       {"project_id", arg_id(a, "project_id", false)}, {"run", arg_bool(a, "run", true)}};
             if (a.contains("wake")) {
                 const json wb = wake_body(a);  // a named copy: .items() on a temporary dangles
                 for (auto& [k, v] : wb.items()) b[k] = v;
             }
             json t = api_ok(api, "POST", "/api/tasks", b);
             std::string out = "task_id: " + t.value("id", "") + "\nchat_id: " + t.value("conversation_id", "") + "\n";
             std::string w = wake_text(t);
             if (!w.empty()) out += "wakes: " + w + "\n";
             int wait = arg_int(a, "wait_seconds", 0, 0, kMaxWait);
             if (wait > 0 && arg_bool(a, "run", true)) out += "\n" + reply_text(api, t.value("conversation_id", ""), wait);
             else out += "Started. Read the result later with get_reply or read_chat (chat_id above).";
             return out;
         }},
        {"run_task", "Run an existing task again, optionally with a note for the ghost.",
         obj({{"task_id", s_str("The task")}, {"note", s_str("Optional extra instruction")}, {"wait_seconds", s_int("Seconds to wait for the answer (default 0)")}},
             {"task_id"}),
         [](const json& a, const AssistantApi& api) {
             json t = api_ok(api, "POST", "/api/tasks/" + arg_id(a, "task_id") + "/run", {{"note", arg_str(a, "note", false, 4000)}});
             std::string conv = t.value("conversation_id", "");
             int wait = arg_int(a, "wait_seconds", 0, 0, kMaxWait);
             if (wait > 0) return reply_text(api, conv, wait);
             return "Started. chat_id: " + conv + " (read the result with get_reply).";
         }},
        {"set_task_wake", "Change a task's wake-up: wake=none, interval (every_min) or daily (at HH:MM). on=false pauses it.",
         obj({{"task_id", s_str("The task")}, {"wake", s_str(kWakeHelp)}, {"every_min", s_int("Minutes (interval)")},
              {"at", s_str("HH:MM (daily)")}, {"on", s_bool("Active (default true)")}},
             {"task_id", "wake"}),
         [](const json& a, const AssistantApi& api) {
             json b = wake_body(a);
             b["on"] = arg_bool(a, "on", true);
             json t = api_ok(api, "PUT", "/api/tasks/" + arg_id(a, "task_id") + "/wake", b);
             std::string w = wake_text(t);
             return w.empty() ? std::string("The task no longer wakes by itself.") : "The task wakes " + w + ".";
         }},
        {"list_scripts", "List the visual scripts (flows of ghost prompts, conditions, delays and loops).", obj(json::object()),
         [](const json&, const AssistantApi& api) {
             json list = api_ok(api, "GET", "/api/scripts").value("scripts", json::array());
             std::string out;
             for (const json& s : list) {
                 std::string w = wake_text(s);
                 int errs = s.value("error_count", 0);
                 out += "- " + s.value("name", "") + " (script_id: " + s.value("id", "") + ")" + (s.value("draft", false) ? " [draft]" : "") +
                        (errs ? " [" + std::to_string(errs) + " problems]" : "") + (w.empty() ? "" : ", wakes " + w) +
                        (s.value("description", "").empty() ? "" : "\n  " + truncate_utf8(s.value("description", ""), 300)) + "\n";
             }
             return out.empty() ? std::string("No scripts yet.") : out;
         }},
        {"script_format", "How a visual script is written (JSON format, block types, expressions). Read it before save_script.", obj(json::object()),
         [](const json&, const AssistantApi& api) {
             return "You save scripts with the save_script tool (pass the JSON object as `script`), not with a <script> tag. "
                    "The guide below was written for ghosts; everything else in it applies to you.\n\n" + api.script_format;
         }},
        {"get_script", "Read a visual script as JSON (to change it with save_script), with its problems.",
         obj({{"script_id", s_str("The script")}}, {"script_id"}),
         [](const json& a, const AssistantApi& api) { return pretty(api_ok(api, "GET", "/api/scripts/" + arg_id(a, "script_id"))); }},
        {"save_script",
         "Create a visual script, or replace one (script_id). `script` is the JSON object described by script_format "
         "(description, project_id, vars, nodes, edges). Returns its problems; only a script without problems can run.",
         obj({{"name", s_str("Script name")}, {"script", {{"type", "object"}, {"description", "The script JSON (see script_format)"}}},
              {"script_id", s_str("Replace this script instead of creating a new one")}},
             {"name", "script"}),
         [](const json& a, const AssistantApi& api) {
             if (!a.contains("script") || !a["script"].is_object()) throw ToolError{"`script` must be a JSON object (see script_format)."};
             json b = a["script"];
             b["name"] = arg_str(a, "name", true, 80);
             std::string id = arg_id(a, "script_id", false);
             json s = id.empty() ? api_ok(api, "POST", "/api/scripts", b) : api_ok(api, "PUT", "/api/scripts/" + id, b);
             std::string out = "script_id: " + s.value("id", "") + "\n";
             json errs = s.contains("check") ? s["check"].value("errors", json::array()) : json::array();
             if (errs.empty()) out += "No problems: it can run (run_script).";
             else {
                 out += "Saved, but it cannot run until these problems are fixed:\n";
                 for (const json& e : errs) out += "- " + (e.is_string() ? e.get<std::string>() : e.dump()) + "\n";
             }
             return out;
         }},
        {"run_script", "Start a visual script. Returns a run_id; follow it with get_script_run.",
         obj({{"script_id", s_str("The script")}}, {"script_id"}),
         [](const json& a, const AssistantApi& api) {
             json r = api_ok(api, "POST", "/api/scripts/" + arg_id(a, "script_id") + "/run", json::object());
             return "run_id: " + r.value("id", "") + "\nstatus: " + r.value("status", "") + "\nFollow it with get_script_run.";
         }},
        {"get_script_run",
         "The state of a script run: status, variables, each block's state and output, and Ask blocks waiting for an answer.",
         obj({{"run_id", s_str("The run")}}, {"run_id"}),
         [](const json& a, const AssistantApi& api) {
             json st = api_ok(api, "GET", "/api/script_runs/" + arg_id(a, "run_id"));
             std::string out = "script: " + st.value("script_name", "") + "\nstatus: " + st.value("status", "") + "\n";
             if (!st.value("error", "").empty()) out += "error: " + st.value("error", "") + "\n";
             if (st.contains("vars") && !st["vars"].empty()) out += "variables: " + dumps(st["vars"]) + "\n";
             out += "blocks:\n";
             const json nodes = st.contains("script") ? st["script"].value("nodes", json::array()) : json::array();
             for (const json& n : nodes) {
                 std::string id = n.value("id", "");
                 const json ns = st.contains("nodes") && st["nodes"].contains(id) ? st["nodes"][id] : json::object();
                 out += "- " + id + " (" + n.value("type", "") + (n.value("label", "").empty() ? "" : ", " + n.value("label", "")) + "): " +
                        ns.value("state", "");
                 if (!ns.value("question", "").empty() && ns.value("state", "") == "running")
                     out += "\n  WAITING FOR AN ANSWER: " + ns.value("question", "") + " (answer_script_ask with node=" + id + ")";
                 if (ns.contains("output") && ns["output"].is_string() && !ns["output"].get<std::string>().empty())
                     out += "\n  output: " + truncate_utf8(ns["output"].get<std::string>(), 4000);
                 if (!ns.value("error", "").empty()) out += "\n  error: " + ns.value("error", "");
                 out += "\n";
             }
             return truncate_utf8(out, kMaxAnswer);
         }},
        {"answer_script_ask", "Answer an Ask block of a script run you started: approved true/false, or a value for value questions.",
         obj({{"run_id", s_str("The run")}, {"node", s_str("The Ask block id")}, {"approved", s_bool("Yes or no")}, {"value", s_str("The value (value questions)")}},
             {"run_id", "node"}),
         [](const json& a, const AssistantApi& api) {
             json st = api_ok(api, "POST", "/api/script_runs/" + arg_id(a, "run_id") + "/answer",
                              {{"node", arg_str(a, "node", true, 32)}, {"approved", arg_bool(a, "approved", false)}, {"value", arg_str(a, "value", false, 2000)}});
             return "Answered. The run is " + st.value("status", "") + ".";
         }},
        {"stop_script_run", "Stop a script run you started.", obj({{"run_id", s_str("The run")}}, {"run_id"}),
         [](const json& a, const AssistantApi& api) {
             json st = api_ok(api, "POST", "/api/script_runs/" + arg_id(a, "run_id") + "/stop", json::object());
             return "The run is " + st.value("status", "") + ".";
         }},
        {"search", "Search chats, tasks, project notes and run errors (case and Turkish letters ignored).",
         obj({{"query", s_str("Words to find (2+ characters)")}}, {"query"}),
         [](const json& a, const AssistantApi& api) {
             std::string q = arg_str(a, "query", true, 200), enc;
             for (unsigned char ch : q) {
                 if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') enc += (char)ch;
                 else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", ch); enc += buf; }
             }
             return pretty(api_ok(api, "GET", "/api/search?q=" + enc));
         }},
    };
    return list;
}

const char* kInstructions =
    "Phantom Co. is a company of AI ghosts (agents) run by people. You act for the person who made your API key, with an editor's rights. "
    "Start with list_ghosts. Talk to a ghost with ask_ghost (keep the chat_id to go on); give lasting or repeated work with create_task "
    "(optionally waking by itself); build multi-step flows as visual scripts (script_format, save_script, run_script, get_script_run). "
    "Ghosts can take minutes: when an answer says 'still working', call get_reply later instead of asking again.";

json rpc_result(const json& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }
json rpc_error(const json& id, int code, const std::string& msg) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}};
}

json handle_one(const json& m, const AssistantApi& api) {
    if (!m.is_object() || !m.contains("method") || !m["method"].is_string()) {
        if (m.is_object() && (m.contains("result") || m.contains("error"))) return nullptr;  // a reply from the client: nothing to say
        return rpc_error(m.is_object() && m.contains("id") ? m["id"] : json(nullptr), -32600, "Invalid request");
    }
    const std::string method = m["method"];
    if (!m.contains("id")) return nullptr;  // notifications (initialized, cancelled...)
    const json id = m["id"];
    const json params = m.contains("params") && m["params"].is_object() ? m["params"] : json::object();

    if (method == "initialize") {
        static const std::set<std::string> known = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
        std::string v = params.value("protocolVersion", "");
        return rpc_result(id, {{"protocolVersion", known.count(v) ? v : "2025-06-18"},
                               {"capabilities", {{"tools", {{"listChanged", false}}}}},
                               {"serverInfo", {{"name", "phantom-co"}, {"title", "Phantom Co."}, {"version", api.version}}},
                               {"instructions", kInstructions}});
    }
    if (method == "ping") return rpc_result(id, json::object());
    if (method == "tools/list") {
        json list = json::array();
        for (const Tool& t : tools()) list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
        return rpc_result(id, {{"tools", list}});
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        const json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
        for (const Tool& t : tools()) {
            if (name != t.name) continue;
            std::string text;
            bool error = false;
            try {
                text = t.run(args, api);
            } catch (const ToolError& e) {
                text = e.text;
                error = true;
            } catch (const std::exception& e) {
                text = std::string("Internal error: ") + e.what();
                error = true;
            }
            return rpc_result(id, {{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", error}});
        }
        return rpc_error(id, -32602, "Unknown tool: " + name);
    }
    if (method == "resources/list") return rpc_result(id, {{"resources", json::array()}});
    if (method == "prompts/list") return rpc_result(id, {{"prompts", json::array()}});
    return rpc_error(id, -32601, "Method not found: " + method);
}

}  // namespace

json assistant_mcp(const json& msg, const AssistantApi& api) {
    if (!msg.is_array()) return handle_one(msg, api);
    json out = json::array();
    for (const json& m : msg) {
        json r = handle_one(m, api);
        if (!r.is_null()) out.push_back(r);
    }
    return out.empty() ? json(nullptr) : out;
}

}  // namespace pc2
