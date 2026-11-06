#pragma once
// Runs visual scripts (Development/Docs/visual_scripts.md): walks the graph, starts prompt blocks as Runner jobs,
// keeps the state of every run in Script_Runs/<id>.json (rewritten after each change) so a run goes on after a restart.
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "runner.hpp"
#include "script_doc.hpp"

namespace pc2 {

class ScriptRunner {
public:
    // role_of: the current role of a person ("" = gone). Prompt blocks run with the rights of the person who started the run.
    ScriptRunner(Store& store, Runner& runner, std::function<std::string(const std::string&)> role_of);
    ~ScriptRunner();
    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    // Picks up runs that were going when the program closed and starts the clock (delays). Call once.
    void start();
    void shutdown();

    // Values of built-in variables other than now_time / today (usage, tokens). false = not known.
    void set_builtin(std::function<bool(const std::string& name, script::Value& out)> f);
    // Subscription usage for claude_left_5h / claude_left_week / codex_left (Auto sign-ins); may be null.
    void set_usage(Usage* u) { usage_ = u; }

    // Starts a run of a checked script. "" + err when it cannot start.
    std::string run(const json& script, const std::string& user_id, std::string& err);
    // stop | pause | resume | step
    bool control(const std::string& run_id, const std::string& action, std::string& err);
    std::optional<json> get(const std::string& run_id);  // state + "events"
    std::vector<json> list(const std::string& script_id);
    bool script_active(const std::string& script_id);
    bool any_active();  // a run is going or paused (the tray's Quit asks first)

    void on_job_finished(const Job& job, const std::string& status, const std::string& answer, const std::string& error);

private:
    void advance(json& st);
    void start_node(json& st, const json& node);
    void complete_node(json& st, const std::string& id, const std::string& port);
    void start_round(json& st, const json& node);
    bool finish_rounds(json& st);
    void fail_node(json& st, const std::string& id, const std::string& msg);
    void end_run(json& st, const std::string& status, const std::string& error);
    void finish_prompt(json& st, const json& node);
    bool start_prompt_job(json& st, const json& node, const std::string& prompt, std::string& err);
    script::ValueEnv env_for(const json& st, const std::set<std::string>& names);
    std::string fill_text(const json& st, const std::string& text);
    void log(const json& st, json ev);
    void save(json& st);
    void tick();
    bool wait_for_usage(json& st, const json& node, const std::set<std::string>& names);
    long long minute_ms() const;

    Store& store_;
    Runner& runner_;
    std::function<std::string(const std::string&)> role_of_;
    std::function<bool(const std::string&, script::Value&)> builtin_;
    Usage* usage_ = nullptr;
    std::mutex mu_;
    std::map<std::string, json> active_;  // running or paused runs
    std::thread clock_;
    std::condition_variable cv_;
    bool stopping_ = false, started_ = false;
};

}  // namespace pc2
