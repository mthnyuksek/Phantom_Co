#include "script_runner.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <cwchar>

#include <windows.h>

#include "budget.hpp"

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

constexpr int64_t kMaxRounds = 100;  // a Do N block repeats at most this often (every round can start ghost runs)

// Built-ins read from Usage: which sign-in and which field. Older readings are refreshed before a block uses them.
struct UsageField { const char* brain; const char* field; const char* label; };
const UsageField* usage_field(const std::string& name) {
    static const std::map<std::string, UsageField> m = {
        {"claude_left_5h", {"claude", "left_5h", "Claude's 5-hour limit"}},
        {"claude_left_week", {"claude", "left_week", "Claude's weekly limit"}},
        {"codex_left", {"codex", "left", "Codex's limit"}},
    };
    auto it = m.find(name);
    return it == m.end() ? nullptr : &it->second;
}
constexpr long long kUsageMaxAge = 30 * 60;

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
                if (ns.value("state", "") != "running") continue;
                if (ns.contains("retry_at") || n["type"] == "ask") continue;  // waiting for its next try or for a person: the clock goes on
                if (n["type"] != "prompt" && n["type"] != "delay") {  // was waiting for a usage reading: try again
                    ns.erase("usage_wait");
                    ns["state"] = "ready";
                    continue;
                }
                if (n["type"] != "prompt") continue;
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
            if (!active_status(st.value("status", ""))) break;
            json& ns = st["nodes"][n["id"].get<std::string>()];
            if (ns.value("state", "") != "running") continue;
            if (ns.contains("usage_wait")) {  // a usage reading was being refreshed for this block
                bool still = false;
                for (auto& b : ns["usage_wait"]) still = still || (usage_ && usage_->refreshing(b.get<std::string>()));
                if (still) continue;
                ns.erase("usage_wait");
                ns["usage_tried"] = true;
                start_node(st, n);
                changed = true;
            } else if (n["type"] == "delay" && ns.value("wake_at", 0LL) <= now) {
                complete_node(st, n["id"], "out");
                changed = true;
            } else if (n["type"] == "prompt" && ns.contains("retry_at") && ns.value("retry_at", 0LL) <= now) {
                ns.erase("retry_at");
                std::string err;
                if (!start_prompt_job(st, n, ns.value("prompt", ""), err)) prompt_failed(st, n, err, true);
                changed = true;
            } else if (n["type"] == "ask" && ns.value("ask_until", 0LL) > 0 && ns.value("ask_until", 0LL) <= now) {
                ns["output"] = "no answer in time";
                log(st, {{"kind", "note"}, {"node", n["id"]}, {"text", "Nobody answered in time."}});
                complete_node(st, n["id"], "rejected");
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
std::string ScriptRunner::run(const json& script, const std::string& user_id, std::string& err, const std::string& trigger) {
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
    if (!trigger.empty()) st["trigger"] = trigger;
    for (const json& n : script["nodes"]) st["nodes"][n["id"].get<std::string>()] = {{"state", "waiting"}};
    for (size_t i = 0; i < script["edges"].size(); ++i) st["edges"].push_back("pending");

    std::string id = st["id"];
    active_[id] = st;
    json& s = active_[id];
    log(s, {{"kind", "run_started"}, {"text", trigger == "wake" ? "Started by its wake-up" : "Started"}, {"user", user_id}});
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
                       {"ended", st.value("ended", "")}, {"error", st.value("error", "")}, {"trigger", st.value("trigger", "")}});
    }
    return out;
}

bool ScriptRunner::any_active() {
    std::lock_guard<std::mutex> lk(mu_);
    return !active_.empty();
}

bool ScriptRunner::script_active(const std::string& script_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, st] : active_)
        if (st.value("script_id", "") == script_id) return true;
    return false;
}

json ScriptRunner::pending_asks(const std::string& user_id, bool owner) {
    std::lock_guard<std::mutex> lk(mu_);
    json out = json::array();
    for (auto& [id, st] : active_) {
        if (!owner && st.value("user_id", "") != user_id) continue;
        for (const json& n : st["script"]["nodes"]) {
            if (n["type"] != "ask") continue;
            const json& ns = st["nodes"][n["id"].get<std::string>()];
            if (ns.value("state", "") != "running") continue;
            out.push_back({{"run_id", id}, {"node", n["id"]}, {"script_id", st.value("script_id", "")}, {"script_name", st.value("script_name", "")},
                           {"question", ns.value("question", "")}, {"mode", n["props"].value("mode", "approve")}, {"type", ns.value("value_type", "")},
                           {"user_id", st.value("user_id", "")}, {"started", ns.value("started", "")}});
        }
    }
    return out;
}

bool ScriptRunner::answer_ask(const std::string& run_id, const std::string& node, const std::string& user_id, bool owner, const std::string& by_name,
                              bool approved, const std::string& value, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(run_id);
    if (it == active_.end()) { err = "This run has already ended."; return false; }
    json& st = it->second;
    if (!owner && st.value("user_id", "") != user_id) { err = "Only the person who started this run (or the owner) can answer."; return false; }
    const json* n = find_node(st, node);
    if (!n || (*n)["type"] != "ask" || st["nodes"][node].value("state", "") != "running") { err = "This question is no longer waiting."; return false; }
    json& ns = st["nodes"][node];
    if (approved && (*n)["props"].value("mode", "approve") == "value") {
        const std::string var = (*n)["props"].value("var", "");
        Value cur, v;
        if (!value_from_json(st["vars"][var], cur) || !parse_value_loose(cur.type, value, v)) { err = "That is not " + value_hint(cur.type) + "."; return false; }
        st["vars"][var] = value_to_json(v);
        ns["output"] = format_value(v);
        log(st, {{"kind", "var_set"}, {"node", node}, {"var", var}, {"text", format_value(v)}});
    } else {
        ns["output"] = approved ? "approved" : "rejected";
    }
    ns["answered_by"] = by_name;
    log(st, {{"kind", "note"}, {"node", node}, {"text", (approved ? "Approved by " : "Rejected by ") + by_name}});
    complete_node(st, node, approved ? "approved" : "rejected");
    advance(st);
    save(st);
    if (!active_status(st.value("status", ""))) active_.erase(it);
    return true;
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
    if (ns.value("state", "") != "running" || ns.contains("retry_at")) return;
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
        if (active_status(st.value("status", "")) && finish_rounds(st)) changed = true;
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
    if (!ns.value("usage_tried", false)) log(st, {{"kind", "node_started"}, {"node", id}, {"text", node_title(n)}});
    if (type == "set" || type == "if" || type == "delay") {
        try {
            if (wait_for_usage(st, n, expr_vars(parse_expr(p.value("expr", ""))))) return;  // the clock thread starts it again
        } catch (const ScriptError&) { /* reported below */ }
    }
    try {
        if (type == "start") {
            complete_node(st, id, "out");
        } else if (type == "end") {
            // like return: the run is done at once, also inside a loop; blocks still working are stopped
            complete_node(st, id, "out");
            end_run(st, "done", "");
        } else if (type == "do") {
            Expr e = parse_expr(p.value("count", ""));
            Value v = eval_expr(e, env_for(st, expr_vars(e)));
            if (v.type != Type::Number || v.i % 100 != 0) throw ScriptError("The number of rounds must be a whole Number.");
            const int64_t rounds = v.i / 100;  // (not `n`: that is the block)
            if (rounds < 0 || rounds > kMaxRounds) throw ScriptError("The number of rounds must be between 0 and " + std::to_string(kMaxRounds) + ".");
            ns["count"] = rounds;
            ns["round"] = 0;
            if (rounds == 0) { ns["output"] = "0 rounds"; complete_node(st, id, "then"); }
            else start_round(st, n);
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
            ns["prompt"] = prompt;  // a retry sends the same text
            std::string err;
            if (!start_prompt_job(st, n, prompt, err)) prompt_failed(st, n, err, true);
        } else if (type == "ask") {
            const std::string question = fill_text(st, p.value("question", ""));
            ns["question"] = question;
            if (p.value("mode", "approve") == "value") {
                Value cur;
                value_from_json(st["vars"][p.value("var", "")], cur);
                ns["value_type"] = type_name(cur.type);
            }
            if (const int minutes = p.value("timeout", 0); minutes > 0) ns["ask_until"] = now_ms() + minutes * minute_ms();
            if (notify_) {
                const std::string name = st.value("script_name", "");
                notify_->post({"ask", st.value("user_id", ""), "A script asks you: " + name, question, "Bir betik soruyor: " + name, question,
                               "#/scripts/" + st.value("script_id", "") + "/run/" + st.value("id", ""), ""});
            }
        }
    } catch (const ScriptError& e) {
        fail_node(st, id, e.what());
    } catch (const std::exception& e) {  // a bug must end the run cleanly, not break the request that started it
        fail_node(st, id, std::string("Internal error: ") + e.what());
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
    if (!last.is_object()) { prompt_failed(st, n, "The ghost gave no answer.", true); return; }
    std::string status = last.value("status", "");
    if (status != "done") {
        std::string why = last.value("error", "");
        // trying again cannot help when a person stopped it or the money is gone
        prompt_failed(st, n, status == "cancelled" ? "The ghost was stopped." : (why.empty() ? "The ghost could not finish (" + status + ")." : why),
                      status != "cancelled" && !is_budget_error(why));
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
                if (!start_prompt_job(st, n, ask, err)) prompt_failed(st, n, err, true);
                return;
            }
            prompt_failed(st, n, "The ghost did not give a usable value for \"" + target + "\" (" + value_hint(cur.type) + ").", true);
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
    bool is_if = n && (*n)["type"] == "if", is_do = n && (*n)["type"] == "do";
    const bool ported = is_if || is_do || (n && ((*n)["type"] == "prompt" || (*n)["type"] == "ask"));  // only the arrows of one output fire
    for (size_t i = 0; i < script["edges"].size(); ++i) {
        const json& e = script["edges"][i];
        if (e["from"] != id) continue;
        if (is_do && e.value("port", "out") == "loop") {  // the rounds are over: the loop arrows stay as they were
            if (ns.value("count", 0) == 0) st["edges"][i] = "skipped";
            continue;
        }
        st["edges"][i] = !ported || e.value("port", "out") == port ? "fired" : "skipped";
    }
    json ev = {{"kind", "node_done"}, {"node", id}};
    if (is_if) ev["text"] = port;
    log(st, ev);
}

// Do N: one more round. The loop part goes back to "waiting" (its arrows to pending) and the loop arrows fire again.
void ScriptRunner::start_round(json& st, const json& n) {
    const std::string id = n["id"];
    json& ns = st["nodes"][id];
    int64_t round = ns.value("round", 0) + 1, count = ns.value("count", 0);
    ns["round"] = round;
    ns["output"] = "round " + std::to_string(round) + " of " + std::to_string(count);
    std::string var = n["props"].value("var", "");
    if (!var.empty() && st["vars"].contains(var)) {
        st["vars"][var] = value_to_json(Value::number(round * 100));
        log(st, {{"kind", "var_set"}, {"node", id}, {"var", var}, {"text", std::to_string(round) + ".00"}});
    }
    const json& script = st["script"];
    std::set<std::string> body = loop_body(script, id);
    for (const std::string& b : body) {
        json fresh = {{"state", "waiting"}};
        if (st["nodes"][b].contains("runs")) fresh["runs"] = st["nodes"][b]["runs"];  // every round's ghost runs stay listed
        st["nodes"][b] = fresh;
    }
    for (size_t i = 0; i < script["edges"].size(); ++i) {
        const json& e = script["edges"][i];
        std::string from = e.value("from", "");
        if (from == id && e.value("port", "out") == "loop") st["edges"][i] = "fired";
        else if (body.count(from)) st["edges"][i] = "pending";
    }
    log(st, {{"kind", "note"}, {"node", id}, {"text", "Round " + std::to_string(round) + " of " + std::to_string(count)}});
}

// A Do N block whose loop part has finished: the next round, or its "then" output.
bool ScriptRunner::finish_rounds(json& st) {
    bool changed = false;
    for (const json& n : st["script"]["nodes"]) {
        if (n["type"] != "do") continue;
        json& ns = st["nodes"][n["id"].get<std::string>()];
        if (ns.value("state", "") != "running" || ns.value("count", 0) == 0) continue;
        bool over = true;
        for (const std::string& b : loop_body(st["script"], n["id"])) {
            std::string s = st["nodes"][b].value("state", "");
            if (s != "done" && s != "skipped") { over = false; break; }
        }
        if (!over) continue;
        if (ns.value("round", 0) < ns.value("count", 0)) start_round(st, n);
        else complete_node(st, n["id"], "then");
        changed = true;
    }
    return changed;
}

void ScriptRunner::prompt_failed(json& st, const json& n, const std::string& msg, bool can_retry) {
    const std::string id = n["id"];
    json& ns = st["nodes"][id];
    const json& p = n["props"];
    const int max_tries = p.value("retry", 0), used = ns.value("tries", 0);
    if (can_retry && used < max_tries) {
        const int wait = p.value("retry_wait", 1);
        ns["tries"] = used + 1;
        ns["last"] = nullptr;
        ns.erase("value_retry");
        ns["retry_at"] = now_ms() + wait * minute_ms();
        ns["last_error"] = msg;
        log(st, {{"kind", "note"}, {"node", id}, {"text", "Failed (" + msg + "). Trying again in " + std::to_string(wait) + " min (" + std::to_string(used + 1) + " of " + std::to_string(max_tries) + ")."}});
        return;
    }
    bool has_failed_arrow = false;
    for (const json& e : st["script"]["edges"])
        has_failed_arrow = has_failed_arrow || (e.value("from", "") == id && e.value("port", "out") == "failed");
    if (!has_failed_arrow) { fail_node(st, id, msg); return; }
    ns["error"] = msg;
    ns["took_failed"] = true;
    log(st, {{"kind", "note"}, {"node", id}, {"text", "Failed (" + msg + "): going on through its \"failed\" arrow."}});
    complete_node(st, id, "failed");
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
    if (!notify_) return;
    const std::string name = st.value("script_name", ""), link = "#/scripts/" + st.value("script_id", "") + "/run/" + st.value("id", "");
    if (status == "failed") {
        notify_->post({"failed", st.value("user_id", ""), "Script failed: " + name, error, "Betik başarısız: " + name, error, link, ""});
    } else if (status == "done") {
        const int min_minutes = notify_->done_minutes();
        const long long took = (long long)std::time(nullptr) - iso_to_unix(st.value("created", ""));
        if (min_minutes >= 0 && took >= min_minutes * 60LL)
            notify_->post({"done", st.value("user_id", ""), "Script finished: " + name, "", "Betik bitti: " + name, "", link, ""});
    }
}

// A block that reads a usage value older than 30 min first asks for a fresh reading (one tiny run) and waits for it.
bool ScriptRunner::wait_for_usage(json& st, const json& n, const std::set<std::string>& names) {
    json& ns = st["nodes"][n["id"].get<std::string>()];
    if (!usage_ || ns.value("usage_tried", false)) return false;
    json waits = json::array();
    std::set<std::string> seen;
    for (const std::string& name : names) {
        const UsageField* f = usage_field(name);
        if (!f || seen.count(f->brain) || usage_->fresh(f->brain, kUsageMaxAge)) continue;
        seen.insert(f->brain);
        usage_->refresh(f->brain);
        waits.push_back(f->brain);
    }
    if (waits.empty()) return false;
    ns["usage_wait"] = waits;
    log(st, {{"kind", "note"}, {"node", n["id"]}, {"text", "Reading how much of the usage limit is left (one short run)."}});
    return true;
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
        else if (const UsageField* f = usage_field(name)) {
            auto r = usage_ ? usage_->get(f->brain) : std::nullopt;
            if (!r || !r->contains(f->field) || !(*r)[f->field].is_number())
                throw ScriptError(std::string(f->label) + " is not known: no run of this PC's " + (std::string(f->brain) == "claude" ? "Claude Code" : "Codex") +
                                  " sign-in has reported it (API keys and other brains do not).");
            env[name] = Value::number(static_cast<int64_t>(std::llround((*r)[f->field].get<double>() * 100)));
        } else {
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
