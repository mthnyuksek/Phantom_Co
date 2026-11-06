#include "script_runner.hpp"

#include <chrono>
#include <ctime>
#include <cwchar>

#include <windows.h>

namespace pc2 {

using namespace script;

namespace {

constexpr size_t kMaxOutput = 20000;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::tm local_now() {
    std::time_t t = std::time(nullptr);
    return *std::localtime(&t);
}

Value local_time_value() {
    std::tm lt = local_now();
    return Value::time(lt.tm_hour * 60 + lt.tm_min);
}

Value local_date_value() {
    std::tm lt = local_now();
    return Value::date(days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday));
}

// Milliseconds until the next local time `minutes` (today if still ahead, else tomorrow).
long long ms_until_time(int64_t minutes) {
    std::time_t now = std::time(nullptr);
    std::tm lt = *std::localtime(&now);
    lt.tm_hour = static_cast<int>(minutes / 60);
    lt.tm_min = static_cast<int>(minutes % 60);
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    std::time_t target = std::mktime(&lt);
    if (target <= now) {
        lt.tm_mday += 1;
        lt.tm_isdst = -1;
        target = std::mktime(&lt);
    }
    return (static_cast<long long>(target) - static_cast<long long>(now)) * 1000;
}

const json* find_node(const json& st, const std::string& id) {
    for (const json& n : st["script"]["nodes"])
        if (n["id"] == id) return &n;
    return nullptr;
}

std::string node_title(const json& n) {
    std::string label = n.value("label", "");
    return label.empty() ? n.value("type", "") + " " + n.value("id", "") : label;
}

std::string value_hint(Type t) {
    switch (t) {
        case Type::Number: return "a number like 12.50";
        case Type::Time: return "a time like 16:00";
        case Type::Date: return "a date like 2026-11-16";
        case Type::Bool: return "true or false";
        default: return "a short text";
    }
}

// The last <value>...</value> in the answer that reads as the wanted type.
bool extract_value(const std::string& text, Type t, Value& out) {
    bool found = false;
    size_t k = 0;
    while ((k = text.find("<value>", k)) != std::string::npos) {
        size_t s = k + 7, e = text.find("</value>", s);
        if (e == std::string::npos) break;
        Value v;
        if (parse_value_loose(t, text.substr(s, e - s), v)) { out = v; found = true; }
        k = e + 8;
    }
    return found;
}

bool active_status(const std::string& s) { return s == "running" || s == "paused"; }

}  // namespace

ScriptRunner::ScriptRunner(Store& store, Runner& runner, std::function<std::string(const std::string&)> role_of)
    : store_(store), runner_(runner), role_of_(std::move(role_of)) {}

ScriptRunner::~ScriptRunner() { shutdown(); }

void ScriptRunner::set_builtin(std::function<bool(const std::string&, Value&)> f) {
    std::lock_guard<std::mutex> lk(mu_);
    builtin_ = std::move(f);
}

long long ScriptRunner::minute_ms() const {
    // Tests make a minute shorter (PHANTOM_SCRIPT_MINUTE_MS); never used otherwise.
    wchar_t buf[32];
    DWORD n = GetEnvironmentVariableW(L"PHANTOM_SCRIPT_MINUTE_MS", buf, 32);
    if (n > 0 && n < 32) {
        long long v = std::wcstoll(buf, nullptr, 10);
        if (v > 0) return v;
    }
    return 60000;
}

void ScriptRunner::start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) return;
        started_ = true;
        // runs that were going when the program closed: a prompt block that was working is started once more
        for (json st : store_.script_runs("", 1000)) {
            if (!active_status(st.value("status", ""))) continue;
            std::string id = st["id"];
            active_[id] = st;
            json& s = active_[id];
            log(s, {{"kind", "note"}, {"text", "The program was restarted."}});
            for (const json& n : s["script"]["nodes"]) {
                json& ns = s["nodes"][n["id"].get<std::string>()];
                if (ns.value("state", "") != "running" || n["type"] != "prompt") continue;
                if (ns.value("restarted", false)) {
                    fail_node(s, n["id"], "It was interrupted twice by a restart.");
                    break;
                }
                ns["state"] = "ready";
                ns["restarted"] = true;
                ns["last"] = nullptr;
                log(s, {{"kind", "note"}, {"node", n["id"]}, {"text", "Interrupted by a restart: starting it again."}});
            }
            advance(s);
            save(s);
        }
        for (auto it = active_.begin(); it != active_.end();)
            it = active_status(it->second.value("status", "")) ? std::next(it) : active_.erase(it);
    }
    clock_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stopping_) {
            cv_.wait_for(lk, std::chrono::milliseconds(200));
            if (stopping_) break;
            try { tick(); } catch (const std::exception&) {}
        }
    });
}

void ScriptRunner::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (clock_.joinable()) clock_.join();
}

// under mu_: delays whose time has come
void ScriptRunner::tick() {
    long long now = now_ms();
    std::vector<std::string> ids;
    for (auto& [id, st] : active_) ids.push_back(id);
    for (const std::string& id : ids) {
        json& st = active_[id];
        bool changed = false;
        for (const json& n : st["script"]["nodes"]) {
            if (n["type"] != "delay") continue;
            json& ns = st["nodes"][n["id"].get<std::string>()];
            if (ns.value("state", "") == "running" && ns.value("wake_at", 0LL) <= now) {
                complete_node(st, n["id"], "out");
                changed = true;
            }
        }
        if (!changed) continue;
        advance(st);
        save(st);
        if (!active_status(st.value("status", ""))) active_.erase(id);
    }
}

// --------------------------------------------------------------------- public
std::string ScriptRunner::run(const json& script, const std::string& user_id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopping_) { err = "The program is closing."; return ""; }
    std::string script_id = script.value("id", "");
    for (auto& [id, st] : active_)
        if (st.value("script_id", "") == script_id) { err = "This script is already running. Stop it first."; return ""; }

    json st = {{"id", new_id()}, {"script_id", script_id}, {"script_name", script.value("name", "")}, {"script", script},
               {"user_id", user_id}, {"status", "running"}, {"created", now_iso()}, {"step", 0},
               {"vars", json::object()}, {"nodes", json::object()}, {"edges", json::array()}};
    for (const json& v : script.value("vars", json::array())) {
        Type t = type_from_name(v.value("type", ""));
        std::string init = v.value("init", "");
        Value val;
        if (init.empty() || !parse_value(t, init, val)) {
            switch (t) {
                case Type::Number: val = Value::number(0); break;
                case Type::Time: val = Value::time(0); break;
                case Type::Date: val = local_date_value(); break;
                case Type::Bool: val = Value::boolean(false); break;
                default: val = Value::text("");
            }
        }
        st["vars"][v.value("name", "")] = value_to_json(val);
    }
    for (const json& n : script["nodes"]) st["nodes"][n["id"].get<std::string>()] = {{"state", "waiting"}};
    for (size_t i = 0; i < script["edges"].size(); ++i) st["edges"].push_back("pending");

    std::string id = st["id"];
    active_[id] = st;
    json& s = active_[id];
    log(s, {{"kind", "run_started"}, {"text", "Started"}, {"user", user_id}});
    advance(s);
    save(s);
    if (!active_status(s.value("status", ""))) active_.erase(id);
    return id;
}

bool ScriptRunner::control(const std::string& run_id, const std::string& action, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(run_id);
    if (it == active_.end()) { err = "This run has already ended."; return false; }
    json& st = it->second;
    std::string status = st.value("status", "");
    if (action == "stop") {
        end_run(st, "stopped", "");
    } else if (action == "pause") {
        if (status != "running") { err = "The run is not running."; return false; }
        st["status"] = "paused";
        log(st, {{"kind", "note"}, {"text", "Paused"}});
    } else if (action == "resume") {
        if (status != "paused") { err = "The run is not paused."; return false; }
        st["status"] = "running";
        st["step"] = 0;
        log(st, {{"kind", "note"}, {"text", "Resumed"}});
        advance(st);
    } else if (action == "step") {
        if (status != "paused") { err = "Pause the run first: a step starts one waiting block."; return false; }
        bool ready = false;
        for (auto& [nid, ns] : st["nodes"].items()) ready = ready || ns.value("state", "") == "ready";
        if (!ready) { err = "No block is waiting to start."; return false; }
        st["step"] = st.value("step", 0) + 1;
        advance(st);
    } else {
        err = "Unknown action.";
        return false;
    }
    save(st);
    if (!active_status(st.value("status", ""))) active_.erase(it);
    return true;
}

std::optional<json> ScriptRunner::get(const std::string& run_id) {
    std::optional<json> st;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = active_.find(run_id);
        if (it != active_.end()) st = it->second;
    }
    if (!st) st = store_.script_run_get(run_id);
    if (!st) return std::nullopt;
    json ev = json::array();
    for (auto& e : store_.script_run_events(run_id)) ev.push_back(e);
    (*st)["events"] = ev;
    return st;
}

std::vector<json> ScriptRunner::list(const std::string& script_id) {
    std::vector<json> out;
    for (json st : store_.script_runs(script_id, 50)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = active_.find(st.value("id", ""));
            if (it != active_.end()) st = it->second;
        }
        out.push_back({{"id", st.value("id", "")}, {"script_id", st.value("script_id", "")}, {"script_name", st.value("script_name", "")},
                       {"user_id", st.value("user_id", "")}, {"status", st.value("status", "")}, {"created", st.value("created", "")},
                       {"ended", st.value("ended", "")}, {"error", st.value("error", "")}});
    }
    return out;
}

bool ScriptRunner::script_active(const std::string& script_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, st] : active_)
        if (st.value("script_id", "") == script_id) return true;
    return false;
}

void ScriptRunner::on_job_finished(const Job& job, const std::string& status, const std::string& answer, const std::string& error) {
    if (job.stack.empty() || job.stack.front().kind != "script") return;
    const std::string& ref = job.stack.front().id;
    size_t slash = ref.find('/');
    if (slash == std::string::npos) return;
    std::string run_id = ref.substr(0, slash), node_id = ref.substr(slash + 1);

    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(run_id);
    if (it == active_.end()) return;  // stopped or ended meanwhile
    json& st = it->second;
    const json* node = find_node(st, node_id);
    if (!node || !st["nodes"].contains(node_id)) return;
    json& ns = st["nodes"][node_id];
    if (ns.value("state", "") != "running") return;
    if (job.stack.size() == 1)  // the block's own ghost (first answer, a report from a helper, a retry)
        ns["last"] = {{"status", status}, {"text", truncate_utf8(answer, kMaxOutput)}, {"error", error}, {"run", job.id}};
    // Helpers asked with <send> may still be working, or their report to this ghost may be waiting.
    if (runner_.conversation_busy(ns.value("conv", ""))) { save(st); return; }
    finish_prompt(st, *node);
    advance(st);
    save(st);
    if (!active_status(st.value("status", ""))) active_.erase(it);
}

// --------------------------------------------------------------------- engine
// Dead-path elimination: a block starts when no incoming arrow is pending and at least one fired;
// when all incoming arrows were skipped, the block is skipped too (so a join after an If still runs).
void ScriptRunner::advance(json& st) {
    const json& script = st["script"];
    bool changed = true;
    while (changed && active_status(st.value("status", ""))) {
        changed = false;
        for (const json& n : script["nodes"]) {
            std::string id = n["id"];
            json& ns = st["nodes"][id];
            if (ns.value("state", "") != "waiting") continue;
            bool pending = false, fired = false, any = false;
            for (size_t i = 0; i < script["edges"].size(); ++i) {
                if (script["edges"][i]["to"] != id) continue;
                any = true;
                std::string e = st["edges"][i];
                pending = pending || e == "pending";
                fired = fired || e == "fired";
            }
            if (pending) continue;
            changed = true;
            if (any && !fired) {
                ns["state"] = "skipped";
                for (size_t i = 0; i < script["edges"].size(); ++i)
                    if (script["edges"][i]["from"] == id) st["edges"][i] = "skipped";
                log(st, {{"kind", "node_skipped"}, {"node", id}});
            } else {
                ns["state"] = "ready";
            }
        }
        for (const json& n : script["nodes"]) {
            if (!active_status(st.value("status", ""))) return;
            json& ns = st["nodes"][n["id"].get<std::string>()];
            if (ns.value("state", "") != "ready") continue;
            if (st["status"] == "paused") {
                if (st.value("step", 0) <= 0) continue;
                st["step"] = st.value("step", 0) - 1;
            }
            start_node(st, n);
            changed = true;
        }
    }
    if (!active_status(st.value("status", ""))) return;
    for (auto& [id, ns] : st["nodes"].items()) {
        std::string s = ns.value("state", "");
        if (s == "waiting" || s == "ready" || s == "running") return;
    }
    end_run(st, "done", "");
}

void ScriptRunner::start_node(json& st, const json& n) {
    const std::string id = n["id"], type = n["type"];
    const json& p = n["props"];
    json& ns = st["nodes"][id];
    ns["state"] = "running";
    ns["started"] = now_iso();
    log(st, {{"kind", "node_started"}, {"node", id}, {"text", node_title(n)}});
    try {
        if (type == "start" || type == "end") {
            complete_node(st, id, "out");
        } else if (type == "set") {
            Expr e = parse_expr(p.value("expr", ""));
            Value v = eval_expr(e, env_for(st, expr_vars(e)));
            std::string var = p.value("var", "");
            Value cur;
            if (!value_from_json(st["vars"][var], cur) || cur.type != v.type) throw ScriptError("'" + var + "' takes a " + std::string(type_name(cur.type)));
            st["vars"][var] = value_to_json(v);
            ns["output"] = format_value(v);
            log(st, {{"kind", "var_set"}, {"node", id}, {"var", var}, {"text", format_value(v)}});
            complete_node(st, id, "out");
        } else if (type == "if") {
            Expr e = parse_expr(p.value("expr", ""));
            Value v = eval_expr(e, env_for(st, expr_vars(e)));
            if (v.type != Type::Bool) throw ScriptError("The condition did not give true or false.");
            ns["output"] = v.b ? "true" : "false";
            complete_node(st, id, v.b ? "true" : "false");
        } else if (type == "delay") {
            Expr e = parse_expr(p.value("expr", ""));
            Value v = eval_expr(e, env_for(st, expr_vars(e)));
            long long wait = 0;
            if (p.value("mode", "for") == "until") {
                if (v.type != Type::Time) throw ScriptError("The delay needs a Time.");
                wait = ms_until_time(v.i);
                ns["output"] = "until " + format_value(v);
            } else {
                if (v.type != Type::Number) throw ScriptError("The delay needs a Number of minutes.");
                wait = v.i <= 0 ? 0 : static_cast<long long>((static_cast<__int128>(v.i) * minute_ms()) / 100);
                ns["output"] = format_value(v) + " min";
            }
            ns["wake_at"] = now_ms() + wait;  // the clock thread finishes it
        } else if (type == "prompt") {
            std::string prompt = fill_text(st, p.value("text", ""));
            std::string target = p.value("target", "");
            if (!target.empty()) {
                Value cur;
                value_from_json(st["vars"][target], cur);
                prompt += "\n\n(When you are done, end your answer with the value of \"" + target + "\" (" + value_hint(cur.type) +
                          ") written like this: <value>...</value>)";
            }
            std::string err;
            if (!start_prompt_job(st, n, prompt, err)) fail_node(st, id, err);
        }
    } catch (const ScriptError& e) {
        fail_node(st, id, e.what());
    }
}

bool ScriptRunner::start_prompt_job(json& st, const json& n, const std::string& prompt, std::string& err) {
    const std::string id = n["id"];
    const json& p = n["props"];
    json& ns = st["nodes"][id];
    const std::string user = st.value("user_id", "");
    const std::string role = role_of_ ? role_of_(user) : "";
    if (role != "owner" && role != "editor") { err = "The person who started this script cannot run ghosts any more."; return false; }
    std::string agent_id = p.value("agent_id", "");
    auto agent = store_.agent(agent_id);
    if (!agent) { err = "The ghost '" + agent_id + "' no longer exists."; return false; }
    std::string project = p.value("project_id", "");
    if (project.empty()) project = st["script"].value("project_id", "");
    if (!project.empty() && !store_.project(project)) { err = "The project '" + project + "' no longer exists."; return false; }

    std::string conv = ns.value("conv", "");
    if (conv.empty()) {
        std::string label = n.value("label", "");
        conv = store_.conv_create({{"kind", "script"}, {"title", "Script · " + st.value("script_name", "") + " · " + (label.empty() ? agent->value("name", agent_id) : label)},
                                   {"agent_id", agent_id}, {"user_id", user}, {"project_id", project}, {"shared", true},
                                   {"script_id", st.value("script_id", "")}, {"script_run_id", st.value("id", "")}, {"node", id}});
        ns["conv"] = conv;
    }
    store_.conv_append(conv, {{"t", "msg"}, {"from", "user:" + user}, {"text", prompt}, {"via", "script"}});
    Job job;
    job.kind = "script";
    job.agent_id = agent_id;
    job.user_id = user;
    job.user_role = role;
    job.project_id = project;
    job.prompt = prompt;
    job.stack.push_back({"script", st.value("id", "") + "/" + id, conv, false});
    std::string run_id = runner_.enqueue_routed(job, err);
    if (run_id.empty()) return false;
    if (!ns.contains("runs")) ns["runs"] = json::array();
    ns["runs"].push_back(run_id);
    return true;
}

// The block's conversation is quiet: take the ghost's last answer (and the value it was asked for).
void ScriptRunner::finish_prompt(json& st, const json& n) {
    const std::string id = n["id"];
    json& ns = st["nodes"][id];
    json last = ns.value("last", json());
    if (!last.is_object()) { fail_node(st, id, "The ghost gave no answer."); return; }
    std::string status = last.value("status", "");
    if (status != "done") {
        std::string why = last.value("error", "");
        fail_node(st, id, status == "cancelled" ? "The ghost was stopped." : (why.empty() ? "The ghost could not finish (" + status + ")." : why));
        return;
    }
    std::string text = last.value("text", "");
    ns["output"] = text;
    std::string target = n["props"].value("target", "");
    if (!target.empty()) {
        Value cur, v;
        value_from_json(st["vars"][target], cur);
        if (!extract_value(text, cur.type, v)) {
            if (!ns.value("value_retry", false)) {
                ns["value_retry"] = true;
                log(st, {{"kind", "note"}, {"node", id}, {"text", "No usable value in the answer: asking once more."}});
                std::string err;
                std::string ask = "Your answer did not contain a usable value. Answer with only the value of \"" + target + "\" (" +
                                  value_hint(cur.type) + ") written like this: <value>...</value>";
                if (!start_prompt_job(st, n, ask, err)) fail_node(st, id, err);
                return;
            }
            fail_node(st, id, "The ghost did not give a usable value for \"" + target + "\" (" + value_hint(cur.type) + ").");
            return;
        }
        st["vars"][target] = value_to_json(v);
        log(st, {{"kind", "var_set"}, {"node", id}, {"var", target}, {"text", format_value(v)}});
    }
    complete_node(st, id, "out");
}

void ScriptRunner::complete_node(json& st, const std::string& id, const std::string& port) {
    const json& script = st["script"];
    json& ns = st["nodes"][id];
    ns["state"] = "done";
    ns["ended"] = now_iso();
    const json* n = find_node(st, id);
    bool is_if = n && (*n)["type"] == "if";
    for (size_t i = 0; i < script["edges"].size(); ++i) {
        const json& e = script["edges"][i];
        if (e["from"] != id) continue;
        st["edges"][i] = !is_if || e.value("port", "out") == port ? "fired" : "skipped";
    }
    json ev = {{"kind", "node_done"}, {"node", id}};
    if (is_if) ev["text"] = port;
    log(st, ev);
}

void ScriptRunner::fail_node(json& st, const std::string& id, const std::string& msg) {
    json& ns = st["nodes"][id];
    ns["state"] = "failed";
    ns["ended"] = now_iso();
    ns["error"] = msg;
    log(st, {{"kind", "node_failed"}, {"node", id}, {"text", msg}});
    const json* n = find_node(st, id);
    end_run(st, "failed", (n ? node_title(*n) : id) + ": " + msg);
}

// A failed block ends the whole run: nothing new starts, ghosts still working are stopped.
void ScriptRunner::end_run(json& st, const std::string& status, const std::string& error) {
    if (!active_status(st.value("status", ""))) return;
    st["status"] = status;
    st["ended"] = now_iso();
    if (!error.empty()) st["error"] = error;
    for (auto& [id, ns] : st["nodes"].items()) {
        std::string s = ns.value("state", "");
        if (s == "running") {
            if (!ns.value("conv", "").empty()) runner_.cancel_conversation(ns["conv"]);
            ns["state"] = "stopped";
            ns["ended"] = now_iso();
        }
    }
    log(st, {{"kind", "run_ended"}, {"text", status}, {"error", error}});
}

ValueEnv ScriptRunner::env_for(const json& st, const std::set<std::string>& names) {
    ValueEnv env;
    for (auto& [name, vj] : st["vars"].items()) {
        Value v;
        if (value_from_json(vj, v)) env[name] = v;
    }
    for (const std::string& name : names) {
        if (env.count(name) || !builtin_types().count(name)) continue;
        if (name == "now_time") env[name] = local_time_value();
        else if (name == "today") env[name] = local_date_value();
        else {
            Value v;
            if (!builtin_ || !builtin_(name, v)) throw ScriptError("'" + name + "' is not known yet.");
            env[name] = v;
        }
    }
    return env;
}

// {name} -> the value as text; unknown names stay as they are.
std::string ScriptRunner::fill_text(const json& st, const std::string& text) {
    std::set<std::string> names;
    for (auto& n : text_placeholders(text)) names.insert(n);
    ValueEnv env;
    try { env = env_for(st, names); } catch (const ScriptError&) {
        // a built-in that is not known: fill what we can
        std::set<std::string> known;
        for (auto& n : names) if (!builtin_types().count(n) || n == "now_time" || n == "today") known.insert(n);
        env = env_for(st, known);
    }
    std::string out;
    size_t k = 0;
    while (k < text.size()) {
        size_t open = text.find('{', k);
        if (open == std::string::npos) { out += text.substr(k); break; }
        size_t close = text.find('}', open + 1);
        if (close == std::string::npos) { out += text.substr(k); break; }
        std::string name = text.substr(open + 1, close - open - 1);
        auto it = env.find(name);
        if (it != env.end()) {
            out += text.substr(k, open - k) + format_value(it->second);
            k = close + 1;
        } else {
            out += text.substr(k, open + 1 - k);
            k = open + 1;
        }
    }
    return out;
}

void ScriptRunner::log(const json& st, json ev) {
    ev["ts"] = now_iso();
    store_.script_run_log(st.value("id", ""), ev);
}

void ScriptRunner::save(json& st) { store_.script_run_save(st); }

}  // namespace pc2
