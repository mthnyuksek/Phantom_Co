#include "runner.hpp"

#include <algorithm>
#include <chrono>

namespace pc2 {

using namespace std::chrono_literals;

static constexpr int kWorkersPerLane = 4;  // upper bound of the max_concurrent setting

Runner::Runner(Store& store) : store_(store) {
    store_.recover_interrupted();
    for (const char* p : {"claude", "codex", "gemini", "opencode", "demo"}) {
        auto lane = std::make_unique<Lane>();
        Lane* raw = lane.get();
        for (int i = 0; i < kWorkersPerLane; ++i) lane->workers.emplace_back([this, raw] { worker_loop(*raw); });
        lanes_[p] = std::move(lane);
    }
}

Runner::~Runner() { shutdown(); }

void Runner::shutdown() {
    if (stopping_.exchange(true)) return;
    hub_.shutdown();
    {
        std::lock_guard<std::mutex> lk(reg_mu_);
        for (auto& [k, js] : registry_) {
            js->cancel = true;
            std::lock_guard<std::mutex> l2(js->mu);
            if (js->proc) js->proc->kill();
        }
    }
    std::vector<Lane*> all;
    { std::lock_guard<std::mutex> lk(lanes_mu_); for (auto& [name, lane] : lanes_) all.push_back(lane.get()); }
    for (Lane* lane : all) {
        { std::lock_guard<std::mutex> lk(lane->mu); }
        lane->cv.notify_all();
        for (auto& w : lane->workers) if (w.joinable()) w.join();
    }
}

// "claude@<user id>" = the Claude Code of a friend whose program lends it to this company.
static bool split_provider(const std::string& full, std::string& base, std::string& worker) {
    size_t at = full.find('@');
    if (at == std::string::npos) { base = full; worker.clear(); return false; }
    base = full.substr(0, at);
    worker = full.substr(at + 1);
    return true;
}

// "claude#<account id>" = Claude Code with another sign-in of this PC (Accounts). "" account = Auto.
static std::string split_account(std::string full, std::string& base) {  // by value: callers pass base as full
    size_t h = full.find('#');
    base = h == std::string::npos ? full : full.substr(0, h);
    return h == std::string::npos ? "" : full.substr(h + 1);
}

Runner::Lane* Runner::lane_for(const std::string& provider) {
    std::lock_guard<std::mutex> lk(lanes_mu_);
    auto it = lanes_.find(provider);
    if (it != lanes_.end()) return it->second.get();
    if (provider.find('#') != std::string::npos) {  // an account: its own lane, so two sign-ins never wait for each other
        std::string base, acc = split_account(provider, base);
        auto a = accounts_ ? accounts_->get(acc) : std::nullopt;
        if (stopping_ || !a || (*a)["provider"] != base) return nullptr;
        auto lane = std::make_unique<Lane>();
        Lane* raw = lane.get();
        for (int i = 0; i < kWorkersPerLane; ++i) raw->workers.emplace_back([this, raw] { worker_loop(*raw); });
        lanes_[provider] = std::move(lane);
        return raw;
    }
    std::string base, worker;
    if (stopping_ || !split_provider(provider, base, worker) || worker.empty() || (base != "claude" && base != "codex")) return nullptr;
    auto lane = std::make_unique<Lane>();
    Lane* raw = lane.get();
    raw->cap = 1;
    for (int i = 0; i < 2; ++i) raw->workers.emplace_back([this, raw] { worker_loop(*raw); });
    lanes_[provider] = std::move(lane);
    return raw;
}

std::shared_ptr<JobState> Runner::find(const std::string& id) {
    std::lock_guard<std::mutex> lk(reg_mu_);
    auto it = registry_.find(id);
    return it == registry_.end() ? nullptr : it->second;
}

// ----------------------------------------------------------------- queueing
std::string Runner::enqueue(Job job, std::string& error) { return enqueue_internal(std::move(job), error, true); }

std::string Runner::enqueue_internal(Job job, std::string& error, bool user_started) {
    auto agent = store_.agent(job.agent_id);
    if (!agent) { error = "This agent no longer exists."; return ""; }
    if (!agent->value("enabled", true)) { error = "This agent is switched off."; return ""; }
    if (job.stack.empty()) { error = "Internal error: empty reply stack."; return ""; }
    const std::string provider = agent->value("provider", "claude");
    Lane* lane = lane_for(provider);
    if (!lane && provider.find('#') != std::string::npos) { error = "The account this ghost used was removed. Pick another brain for it."; return ""; }
    if (!lane) { error = "Unknown provider: " + provider; return ""; }

    job.id = new_id();
    auto js = std::make_shared<JobState>();
    js->job = job;
    js->brain = provider;
    {
        // The duplicate checks and the registration happen under one lock: two requests arriving at the same
        // moment (double click, manual run + wake-up) cannot both pass the check.
        std::lock_guard<std::mutex> lk(reg_mu_);
        int mine = 0;
        for (auto& [k, o] : registry_) {
            if (o->finished) continue;
            if (o->job.user_id == job.user_id) ++mine;
            if (user_started) {
                // A person never gives work to a conversation that is already being worked on.
                for (auto& f : o->job.stack)
                    if (f.conv == job.conv()) { error = "The agent is still working here."; return ""; }
            } else if (o->job.kind == job.kind && o->job.agent_id == job.agent_id && o->job.stack.size() == job.stack.size() &&
                       o->job.stack.back().conv == job.stack.back().conv && o->job.prompt == job.prompt) {
                error = "The same request is already being worked on.";  // identical report/request still pending
                return "";
            }
        }
        if (user_started) {
            int limit = store_.setting_int("max_queue_per_user");
            if (limit > 0 && mine >= limit) { error = "You already have " + std::to_string(mine) + " runs waiting. Wait for some to finish."; return ""; }
        }
        registry_[job.id] = js;
        if (registry_.size() > 300)  // keep memory small: forget the oldest finished runs (their logs stay on disk)
            for (auto it = registry_.begin(); it != registry_.end() && registry_.size() > 200;)
                it = it->second->finished ? registry_.erase(it) : std::next(it);
    }
    json rec = {{"id", job.id}, {"kind", job.kind}, {"task_id", job.task_id}, {"agent_id", job.agent_id}, {"user_id", job.user_id},
                {"provider", provider}, {"model", agent->value("model", "")}, {"conversation_id", job.conv()},
                {"status", "queued"}, {"created", now_iso()}};
    store_.run_save(rec);
    {
        std::lock_guard<std::mutex> lk(js->mu);
        js->events_file.open(store_.run_events_path(job.id), std::ios::app | std::ios::binary);
    }
    emit(*js, {{"kind", "status"}, {"text", "Queued"}});
    {
        std::lock_guard<std::mutex> lk(lane->mu);
        lane->queue.push_back(js);
    }
    lane->cv.notify_all();
    return job.id;
}

std::deque<std::shared_ptr<JobState>>::iterator Runner::pick(Lane& lane) {
    int limit = lane.cap > 0 ? lane.cap : std::clamp(store_.setting_int("max_concurrent"), 1, kWorkersPerLane);
    if (lane.running >= limit) return lane.queue.end();
    return std::find_if(lane.queue.begin(), lane.queue.end(), [&](const std::shared_ptr<JobState>& j) {
        return !lane.running_agents.count(j->job.agent_id);
    });
}

void Runner::worker_loop(Lane& lane) {
    while (true) {
        std::shared_ptr<JobState> js;
        {
            std::unique_lock<std::mutex> lk(lane.mu);
            lane.cv.wait(lk, [&] { return stopping_ || pick(lane) != lane.queue.end(); });
            if (stopping_) return;
            auto it = pick(lane);
            js = *it;
            lane.queue.erase(it);
            lane.running_agents.insert(js->job.agent_id);
            ++lane.running;
        }
        try {
            execute(js);
        } catch (const std::exception& e) {
            // Close the run like any failure, so the record does not stay "running" and the asker (person or agent) hears of it.
            Outcome o;
            o.status = "failed";
            o.error = std::string("Internal error: ") + e.what();
            emit(*js, {{"kind", "error"}, {"text", o.error}});
            { std::lock_guard<std::mutex> lk(js->mu); js->status = "failed"; }
            try {
                if (auto rec = store_.run_get(js->job.id)) {
                    (*rec)["status"] = "failed";
                    (*rec)["ended"] = now_iso();
                    (*rec)["error"] = o.error;
                    store_.run_save(*rec);
                }
                emit(*js, {{"kind", "done"}, {"status", "failed"}, {"text", o.error}});
                route(js, o);
            } catch (const std::exception&) {}
        }
        js->finished = true;
        { std::lock_guard<std::mutex> lk(js->mu); if (js->events_file.is_open()) js->events_file.close(); }
        {
            std::lock_guard<std::mutex> lk(lane.mu);
            lane.running_agents.erase(js->job.agent_id);
            --lane.running;
        }
        lane.cv.notify_all();
    }
}

// -------------------------------------------------------------- live state
void Runner::emit(JobState& js, json ev) {
    std::lock_guard<std::mutex> lk(js.mu);
    ev["seq"] = js.events.size() + 1;
    ev["ts"] = now_iso();
    if (js.events_file.is_open()) { js.events_file << dumps(ev) << "\n"; js.events_file.flush(); }
    std::string k = ev.value("kind", "");
    if (k == "tool_use") js.activity.push_back("🔧 " + ev.value("name", "") + " " + truncate_utf8(ev.value("text", ""), 80));
    else if (k == "message") js.activity.push_back(truncate_utf8(ev.value("text", ""), 100));
    while (js.activity.size() > 12) js.activity.pop_front();
    js.events.push_back(std::move(ev));
}

void Runner::say(const std::string& conv, const std::string& text) {
    store_.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", text}});
}

json Runner::events(const std::string& run_id, size_t after) {
    json out = {{"events", json::array()}, {"status", "unknown"}};
    if (auto js = find(run_id)) {
        std::lock_guard<std::mutex> lk(js->mu);
        for (size_t i = after; i < js->events.size(); ++i) out["events"].push_back(js->events[i]);
        out["status"] = js->status;
        out["next"] = js->events.size();
        return out;
    }
    auto rec = store_.run_get(run_id);
    if (!rec) { out["status"] = "missing"; return out; }
    out["status"] = rec->value("status", "unknown");
    std::string text;
    size_t idx = 0;
    if (read_file(store_.run_events_path(run_id), text)) {
        size_t pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            json ev = json::parse(text.substr(pos, e - pos), nullptr, false);
            if (ev.is_object()) { if (idx >= after) out["events"].push_back(ev); ++idx; }
            pos = e + 1;
        }
    }
    out["next"] = idx;
    return out;
}

json Runner::active_jobs() {
    json out = json::array();
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished) continue;
        std::lock_guard<std::mutex> l2(js->mu);
        json act = json::array();
        for (size_t i = js->activity.size() > 4 ? js->activity.size() - 4 : 0; i < js->activity.size(); ++i) act.push_back(js->activity[i]);
        out.push_back({{"id", id}, {"kind", js->job.kind}, {"agent_id", js->job.agent_id}, {"user_id", js->job.user_id},
                       {"conversation_id", js->job.conv()}, {"task_id", js->job.task_id}, {"running", js->status == "running"},
                       {"started", js->started}, {"activity", act}});
    }
    return out;
}

json Runner::agent_activity() {
    json out = json::object();
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished) continue;
        bool running;
        { std::lock_guard<std::mutex> l2(js->mu); running = js->status == "running"; }
        const std::string a = js->job.agent_id;
        if (out.contains(a) && out[a]["state"] == "working") continue;
        out[a] = {{"state", running ? "working" : "queued"}, {"conversation_id", js->job.conv()}, {"task_id", js->job.task_id}};
    }
    return out;
}

bool Runner::conversation_busy(const std::string& conv) {
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished) continue;
        for (auto& f : js->job.stack) if (f.conv == conv) return true;
    }
    return false;
}

json Runner::conversation_live(const std::string& conv) {
    json out = json::array();
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished || js->job.conv() != conv) continue;
        std::lock_guard<std::mutex> l2(js->mu);
        json act = json::array();
        for (size_t i = js->activity.size() > 4 ? js->activity.size() - 4 : 0; i < js->activity.size(); ++i) act.push_back(js->activity[i]);
        out.push_back({{"agent_id", js->job.agent_id}, {"run_id", id}, {"status", js->status}, {"activity", act}});
    }
    return out;
}

std::string Runner::task_state(const std::string& conv) {
    std::string state;
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished) continue;
        for (auto& f : js->job.stack) {
            if (f.conv != conv) continue;
            std::lock_guard<std::mutex> l2(js->mu);
            if (js->status == "running") return "running";
            state = "queued";
        }
    }
    return state;
}

bool Runner::brain_busy(const std::string& brain) {
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [k, js] : registry_)
        if (!js->finished && js->brain == brain) return true;
    return false;
}

bool Runner::any_active() {
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) if (!js->finished) return true;
    return false;
}

bool Runner::cancel_run(const std::string& run_id) {
    auto js = find(run_id);
    if (!js || js->finished) return false;
    js->cancel = true;
    std::lock_guard<std::mutex> lk(js->mu);
    if (js->proc) js->proc->kill();
    return true;
}

int Runner::cancel_conversation(const std::string& conv) {
    std::vector<std::shared_ptr<JobState>> hit;
    {
        std::lock_guard<std::mutex> lk(reg_mu_);
        for (auto& [id, js] : registry_) {
            if (js->finished) continue;
            for (auto& f : js->job.stack) if (f.conv == conv) { hit.push_back(js); break; }
        }
    }
    for (auto& js : hit) cancel_run(js->job.id);
    return (int)hit.size();
}

// ---------------------------------------------------------------- execution
static std::string agent_name(Store& s, const std::string& id) {
    auto a = s.agent(id);
    return a ? a->value("name", id) : id;
}

void Runner::execute(const std::shared_ptr<JobState>& js) {
    const Job& job = js->job;
    auto rec_opt = store_.run_get(job.id);
    json rec = rec_opt ? *rec_opt : json{{"id", job.id}};
    Outcome o;

    auto finish = [&](Outcome& out) {
        { std::lock_guard<std::mutex> lk(js->mu); js->status = out.status; }
        rec["status"] = out.status;
        rec["ended"] = now_iso();
        rec["exit_code"] = out.exit_code;
        rec["tokens_in"] = out.tokens_in;
        rec["tokens_out"] = out.tokens_out;
        if (out.cost >= 0) rec["cost"] = std::to_string(out.cost);
        if (!out.error.empty()) rec["error"] = out.error;
        rec["result"] = out.text;
        store_.run_save(rec);
        emit(*js, {{"kind", "done"}, {"status", out.status}, {"text", out.error}});
        route(js, out);
    };

    auto agent = store_.agent(job.agent_id);
    if (js->cancel) { o.status = "cancelled"; finish(o); return; }
    if (!agent) { o.status = "failed"; o.error = "The agent no longer exists."; finish(o); return; }

    rec["status"] = "running";
    rec["started"] = now_iso();
    store_.run_save(rec);
    {
        std::lock_guard<std::mutex> lk(js->mu);
        js->status = "running";
        js->started = rec["started"];
    }
    emit(*js, {{"kind", "status"}, {"text", "Started"}});

    // Context: who asked, which project, what the agent may do.
    json user = job.user_id.empty() ? json() : store_.user(job.user_id).value_or(json());
    auto conv = store_.conv_load(job.conv());
    bool shared = conv && (*conv)["meta"].value("shared", false);
    json project;
    if (!job.project_id.empty()) if (auto p = store_.project(job.project_id)) project = *p;
    fs::path workdir = project.is_object() ? store_.project_dir(job.project_id) : store_.files_root();
    std::string base_provider, worker;
    const bool remote = split_provider(js->brain, base_provider, worker);  // the lane it was queued on, even if the agent changed since
    const std::string account = remote ? "" : split_account(base_provider, base_provider);
    const std::string scope = project.is_object() ? "project:" + job.project_id : "shared";
    std::error_code ec;
    fs::create_directories(workdir, ec);

    AgentContext ctx = resolve_agent(store_, *agent, job.user_role, project.is_object() ? job.project_id : "");
    // MCP servers run only on this PC: never on a friend's, and only the ones that exist and are switched on.
    {
        std::vector<std::string> keep;
        if (!remote && mcp_)
            for (auto& n : ctx.mcp)
                if (auto s = mcp_->get(n); s && s->value("enabled", true)) keep.push_back(n);
        ctx.mcp = keep;
    }
    PromptInput pin;
    pin.ctx = &ctx;
    pin.user_name = user.is_object() ? user.value("name", "") : "";
    pin.shared_chat = shared;
    pin.workdir = remote ? "@@WORKDIR@@" : U8(workdir);  // a friend's PC fills in its own folder
    pin.workdir_label = project.is_object() ? "the folder of project \"" + project.value("name", "") + "\"" : "the shared files folder";
    pin.project = project;

    RunSpec spec;
    spec.provider = base_provider;
    spec.model = agent->value("model", "");
    spec.workdir = U8(workdir);
    spec.system_prompt = build_system_prompt(store_, pin);
    spec.prompt = job.prompt;
    if (job.name_chat) spec.prompt += "\n\n(This is the first message of this conversation. Start your answer with <title>2-4 word title</title>.)";
    spec.tools = ctx.tools;
    spec.add_dirs = ctx.add_dirs;
    spec.can_write = ctx.can_write();
    spec.max_turns = (job.kind == "chat" || job.kind == "task") ? 40 : 25;
    // A session lives on the PC and in the sign-in that ran it: remote ids are stored as "@<worker>:<id>", account ids as
    // "#<account>:<id>", Auto ids plain. Another brain starts a new session.
    if (conv && (*conv)["sessions"].contains(job.agent_id)) {
        std::string sid = (*conv)["sessions"][job.agent_id].get<std::string>();
        const std::string prefix = remote ? "@" + worker + ":" : !account.empty() ? "#" + account + ":" : "";
        if (!prefix.empty()) spec.session_id = sid.rfind(prefix, 0) == 0 ? sid.substr(prefix.size()) : "";
        else spec.session_id = !sid.empty() && (sid[0] == '@' || sid[0] == '#') ? "" : sid;
    }
    if (!account.empty() && accounts_) spec.env = accounts_->env(base_provider, account);
    if (remote) {
        // The owner of this company must not be able to run commands on a friend's PC: only file and web tools, no extra folders.
        static const std::set<std::string> allowed = {"Read", "Write", "Edit", "Glob", "Grep", "WebSearch", "WebFetch"};
        std::vector<std::string> keep;
        for (auto& t : spec.tools) if (allowed.count(t)) keep.push_back(t);
        spec.tools = keep;
        spec.add_dirs.clear();
        spec.can_write = std::find(keep.begin(), keep.end(), "Write") != keep.end() || std::find(keep.begin(), keep.end(), "Edit") != keep.end();
        // Agents on this PC hand over host paths ("C:\Users\...\Projects\x\Assets"): point them at the friend's mirror instead.
        const std::string host_dir = U8(workdir);
        spec.prompt = replace_all(replace_all(spec.prompt, host_dir, "@@WORKDIR@@"), replace_all(host_dir, "\\", "/"), "@@WORKDIR@@");
    }
    if (spec.model.empty() && spec.provider == "claude") spec.model = store_.settings().value("default_model", "");  // a Claude model name
    // Gemini's "auto" picks a Pro model, which has no quota on a free API key (the only way left to use Gemini CLI for free).
    if (spec.model.empty() && spec.provider == "gemini") spec.model = kGeminiDefaultModel;
    // OpenCode: the default model of the account's service (Auto: of a service whose key is in the PC's environment).
    if (spec.model.empty() && spec.provider == "opencode") {
        auto acc = account.empty() || !accounts_ ? std::nullopt : accounts_->get(account);
        if (acc) { if (auto sv = opencode_service(acc->value("service", ""))) spec.model = sv->model; }
        else for (auto& sv : opencode_services()) {
            wchar_t buf[8];
            if (spec.model.empty() && GetEnvironmentVariableW(Widen(sv.env).c_str(), buf, 8) > 0) spec.model = sv.model;
        }
    }

    if (spec.provider == "demo") {
        o = run_demo(js, ctx, U8(workdir), spec.system_prompt);
        finish(o);
        return;
    }
    if (remote) {
        o = run_remote(js, spec, worker, scope, std::clamp(agent->value("timeout_min", 30), 1, 240));
        finish(o);
        return;
    }

    fs::path tmp = config_dir() / L"Tmp";
    fs::create_directories(tmp, ec);
    fs::path sysfile = tmp / P(job.id + ".system.md");
    if (spec.provider == "claude" || spec.provider == "gemini" || spec.provider == "opencode") {  // Gemini: GEMINI_SYSTEM_MD, OpenCode: {file:} in its config
        write_file_atomic(sysfile, spec.system_prompt);
        spec.system_file = U8(sysfile);
    }

    // MCP servers: a one-time key lets this run's proxies fetch them (and ask for approvals) while the run lives.
    struct McpGuard {
        Mcp* m = nullptr;
        std::string key;
        fs::path file;
        ~McpGuard() {
            if (m && !key.empty()) m->close_run(key);
            std::error_code e;
            if (!file.empty()) fs::remove(file, e);
        }
    } mcp_guard;
    if (!ctx.mcp.empty()) {
        McpRun r;
        r.run_id = job.id;
        r.agent_id = job.agent_id;
        r.user_id = job.user_id;
        r.conv = job.stack.front().conv;  // the person at the bottom of the reply stack answers approvals
        r.workdir = U8(workdir);
        r.servers.insert(ctx.mcp.begin(), ctx.mcp.end());
        wchar_t self[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        mcp_guard.m = mcp_;
        mcp_guard.key = mcp_->open_run(r);
        mcp_guard.file = tmp / P(job.id + ".mcp.json");
        spec.mcp = ctx.mcp;
        spec.mcp_exe = Narrow(std::wstring(self, n));
        spec.mcp_key = mcp_guard.key;
        spec.mcp_port = mcp_->port();
        spec.mcp_file = U8(mcp_guard.file);
    }
    if (spec.provider == "opencode" && spec.mcp_file.empty()) {  // OpenCode always gets a config file of its own (removed with the run)
        mcp_guard.file = tmp / P(job.id + ".opencode.json");
        spec.mcp_file = U8(mcp_guard.file);
    }

    ProcessSpec ps;
    std::string error;
    if (!build_command(spec, ps, error)) { o.status = "failed"; o.error = error; finish(o); return; }

    const auto started_at = fs::file_time_type::clock::now() - std::chrono::seconds(2);
    Process proc;
    if (!proc.start(ps, error)) { o.status = "failed"; o.error = error; fs::remove(sysfile, ec); finish(o); return; }
    { std::lock_guard<std::mutex> lk(js->mu); js->proc = &proc; }
    if (js->cancel) proc.kill();

    std::mutex wm;
    std::condition_variable wcv;
    bool wdone = false;
    std::atomic<bool> timed_out{false};
    int limit_min = std::clamp(agent->value("timeout_min", 30), 1, 240);
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(wm);
        if (!wcv.wait_for(lk, std::chrono::minutes(limit_min), [&] { return wdone; })) { timed_out = true; proc.kill(); }
    });

    LineParser parser(spec.provider);
    proc.read_lines([&](const std::string& line) {
        const bool stopped = parser.stop_now;
        for (auto& ev : parser.feed(line)) emit(*js, std::move(ev));
        if (parser.stop_now && !stopped) proc.kill();  // a run that cannot succeed (Gemini quota / refused sign-in)
    });
    o.exit_code = proc.wait();
    { std::lock_guard<std::mutex> lk(wm); wdone = true; }
    wcv.notify_all();
    watchdog.join();
    { std::lock_guard<std::mutex> lk(js->mu); js->proc = nullptr; }
    fs::remove(sysfile, ec);

    o.text = parser.result_text;
    if (spec.provider == "codex" && spec.can_write && !js->cancel)
        o.text += images_note(collect_codex_images(parser.session_id, started_at, workdir, account.empty() || !accounts_ ? fs::path() : accounts_->home(account)));
    o.session_id = parser.session_id.empty() || account.empty() ? parser.session_id : "#" + account + ":" + parser.session_id;
    o.tokens_in = parser.tokens_in;
    o.tokens_out = parser.tokens_out;
    o.cost = parser.cost_usd;
    o.max_turns = parser.max_turns;
    if (js->cancel) o.status = "cancelled";
    else if (timed_out) { o.status = "timeout"; o.error = "Stopped after the " + std::to_string(limit_min) + " minute limit."; }
    else if (o.max_turns) o.status = "done";
    else if (o.exit_code != 0 || parser.is_error) {
        o.status = "failed";
        o.error = parser.is_error && !parser.result_text.empty() ? parser.result_text : "The CLI exited with code " + std::to_string(o.exit_code) + ".";
        const std::string& t = o.error;
        auto acc = account.empty() || !accounts_ ? std::nullopt : accounts_->get(account);
        if (acc && (*acc)["kind"] == "api_key") {
            if (t.find("429") == std::string::npos && (t.find("API key") != std::string::npos || t.find("api key") != std::string::npos || t.find("401") != std::string::npos))
                o.error += " (This ghost uses an API key: check or replace it in Settings > Secrets.)";
        } else if (acc && (t.find("/login") != std::string::npos || t.find("ot logged in") != std::string::npos || t.find("codex login") != std::string::npos || t.find("not signed in") != std::string::npos))
            o.error += " (This ghost uses another account: sign in to it in Settings > Secrets.)";
    }
    finish(o);
}

// Runs the job on a friend's PC (its own Claude/Codex sign-in). The files of the folder are mirrored there and back.
Runner::Outcome Runner::run_remote(const std::shared_ptr<JobState>& js, const RunSpec& spec, const std::string& worker, const std::string& scope, int timeout_min) {
    Outcome o;
    if (!store_.user(worker)) { o.status = "failed"; o.error = "The owner of this AI no longer exists."; return o; }
    json payload = {{"provider", spec.provider}, {"model", spec.model}, {"system_prompt", spec.system_prompt}, {"prompt", spec.prompt},
                    {"tools", spec.tools}, {"can_write", spec.can_write}, {"max_turns", spec.max_turns}, {"session_id", spec.session_id},
                    {"timeout_min", timeout_min}, {"scope", scope}, {"run_id", js->job.id}};
    emit(*js, {{"kind", "status"}, {"text", hub_.online(worker) ? "Sent to a friend's PC" : "Waiting for a friend's PC to come online"}});
    static const std::set<std::string> kinds = {"status", "message", "thinking", "tool_use", "tool_result", "log", "error"};
    auto job = hub_.submit(worker, js->job.id, scope, payload, [this, js](json ev) {
        if (!ev.is_object() || !kinds.count(ev.value("kind", ""))) return;
        ev.erase("seq");
        ev.erase("ts");
        for (auto& [k, v] : ev.items()) if (v.is_string() && v.get<std::string>().size() > 20000) v = truncate_utf8(v.get<std::string>(), 20000);
        emit(*js, std::move(ev));
    });
    std::string error;
    bool ok = hub_.wait(job, js->cancel, timeout_min, error);
    if (!ok) {
        o.status = js->cancel || error == "stopped" ? "cancelled" : "failed";
        o.error = error;
        return o;
    }
    json r;
    { std::lock_guard<std::mutex> lk(job->mu); r = job->result; }
    o.status = r.value("status", "failed");
    if (o.status != "done" && o.status != "failed" && o.status != "cancelled" && o.status != "timeout") o.status = "failed";
    o.text = r.value("text", "");
    o.error = r.value("error", "");
    o.exit_code = r.value("exit_code", 0);
    o.tokens_in = r.value("tokens_in", 0LL);
    o.tokens_out = r.value("tokens_out", 0LL);
    o.cost = r.value("cost", -1.0);
    o.max_turns = r.value("max_turns", false);
    std::string sid = r.value("session_id", "");
    if (!sid.empty()) o.session_id = "@" + worker + ":" + sid;
    return o;
}

// A scripted agent that needs no subscription: for trying the app and for tests.
// Keywords in the message steer it: demo:send:<agent_id>, demo:write, demo:slow, demo:fail, demo:prompt (shows the system prompt).
Runner::Outcome Runner::run_demo(const std::shared_ptr<JobState>& js, const AgentContext& ctx, const std::string& workdir, const std::string& system_prompt) {
    Outcome o;
    const Job& job = js->job;
    auto step = [&](json ev, int ms) {
        for (int i = 0; i < ms / 50 && !js->cancel; ++i) std::this_thread::sleep_for(50ms);
        if (!js->cancel) emit(*js, std::move(ev));
    };
    const std::string name = ctx.agent.value("name", "Ghost");
    const std::string& p = job.prompt;
    step({{"kind", "status"}, {"text", "Session started"}, {"model", "demo"}}, 200);
    if (p.find("demo:slow") != std::string::npos) step({{"kind", "thinking"}, {"text", "Thinking very slowly..."}}, 15000);
    if (p.find("demo:fail") != std::string::npos) {
        step({{"kind", "error"}, {"text", "Demo failure requested."}}, 200);
        o.status = "failed";
        o.error = "Demo failure requested.";
        return o;
    }
    std::string text;
    if (job.name_chat) text += "<title>Demo chat</title>";
    if (job.kind == "report") {
        text += "Thanks, I got the report. " + truncate_utf8(p.substr(p.find('\n') == std::string::npos ? 0 : p.find('\n') + 1), 160);
    } else {
        text += "Boo! " + name + " here. You said: " + truncate_utf8(p, 160);
    }
    if (p.find("demo:write") != std::string::npos && ctx.has_tool("Write")) {
        std::string file = "demo_" + job.id.substr(18) + ".md";
        step({{"kind", "tool_use"}, {"name", "Write"}, {"text", file}}, 300);
        if (!js->cancel) write_file_atomic(P(workdir) / P(file), "# Demo\n\nWritten by " + name + ".\n");
        step({{"kind", "tool_result"}, {"is_error", false}, {"text", "Wrote " + file}}, 200);
        text += " I saved " + file + ".";
    }
    if (p.find("demo:prompt") != std::string::npos && job.kind != "report") {
        std::string shown = system_prompt;  // without '<', or the router would act on the <send> example inside it
        for (char& ch : shown) if (ch == '<') ch = '[';
        text += "\n[system prompt]\n" + shown;
    }
    for (const char* kw : {"demo:send:", "demo:tell:"}) {  // tell = information only (reply="no")
        size_t s = p.find(kw);
        if (s == std::string::npos || job.kind == "report") continue;
        std::string target;
        for (size_t i = s + 10; i < p.size() && (isalnum((unsigned char)p[i]) || p[i] == '_' || p[i] == '-'); ++i) target += p[i];
        text += "\n<send to=\"" + target + "\" title=\"Demo request\"" + (kw[5] == 't' ? " reply=\"no\"" : "") + ">Please say hi to the user.</send>";
    }
    step({{"kind", "message"}, {"text", text}}, 300);
    o.text = text;
    o.session_id = "demo-" + job.conv() + "-" + job.agent_id;
    o.tokens_in = 10;
    o.tokens_out = 5;
    if (js->cancel) o.status = "cancelled";
    return o;
}

// ----------------------------------------------------------------- routing
bool Runner::has_pending_request(const std::string& sender, const std::string& target) {
    std::lock_guard<std::mutex> lk(reg_mu_);
    for (auto& [id, js] : registry_) {
        if (js->finished || js->job.kind != "msg" || js->job.agent_id != target) continue;
        const Frame& top = js->job.stack.back();
        if (top.kind == "agent" && top.id == sender) return true;
    }
    return false;
}

void Runner::handle_send(const std::shared_ptr<JobState>& js, const SendBlock& blk, const std::string& sender_name) {
    const Job& job = js->job;
    const std::string conv = job.conv();
    auto target = store_.agent(blk.to);
    if (!target || !target->value("enabled", true) || blk.to == job.agent_id) {
        say(conv, "⚠ " + sender_name + " tried to ask \"" + blk.to + "\", who is not available.");
        return;
    }
    if (job.hops >= store_.setting_int("max_hops")) {
        say(conv, "⚠ " + sender_name + " reached the limit of hand-overs between agents.");
        return;
    }
    if (has_pending_request(job.agent_id, blk.to)) {  // also for information-only sends: never ask a ghost twice while it is still on it
        say(conv, "⚠ " + sender_name + " is already waiting for " + target->value("name", blk.to) + ".");
        return;
    }
    std::string title = blk.title.empty() ? sender_name + " → " + target->value("name", blk.to) : blk.title;
    auto parent = store_.conv_load(conv);  // work done for a shared chat or a task stays visible to the same people
    bool shared = parent && ((*parent)["meta"].value("shared", false) || (*parent)["meta"].value("kind", "") == "task");
    std::string xconv = store_.conv_create({{"kind", "agents"}, {"title", title}, {"participants", json::array({job.agent_id, blk.to})},
                                            {"project_id", job.project_id}, {"user_id", job.user_id}, {"parent", conv}, {"shared", shared}});
    store_.conv_append(xconv, {{"t", "msg"}, {"from", "agent:" + job.agent_id}, {"text", blk.text}});
    Job nj;
    nj.kind = "msg";
    nj.agent_id = blk.to;
    nj.user_id = job.user_id;
    nj.user_role = job.user_role;
    nj.task_id = job.task_id;
    nj.project_id = job.project_id;
    nj.hops = job.hops + 1;
    nj.stack = job.stack;
    nj.stack.push_back({"agent", job.agent_id, xconv, blk.reply});
    nj.prompt = "Request from " + sender_name + ":\n" + blk.text;
    std::string err;
    if (enqueue_internal(nj, err, false).empty()) say(conv, "⚠ Could not ask " + target->value("name", blk.to) + ": " + err);
    else say(conv, "→ " + sender_name + " asked " + target->value("name", blk.to) + (blk.reply ? "" : " (information only)") + ".");
}

void Runner::route(const std::shared_ptr<JobState>& js, const Outcome& o) {
    const Job& job = js->job;
    const std::string name = agent_name(store_, job.agent_id);
    const Frame top = job.stack.back();
    const std::string conv = top.conv;

    if (o.status == "cancelled") { say(conv, "■ " + name + " was stopped."); return; }

    ParsedReply pr;
    std::string text, failure;
    if (o.status == "done") {
        pr = parse_reply(o.text);
        text = pr.text;
    } else {
        failure = "⚠ " + name + " could not finish: " + (o.error.empty() ? o.status : o.error);
        say(conv, failure);
    }
    if (!o.session_id.empty()) store_.conv_append(conv, {{"t", "session"}, {"agent", job.agent_id}, {"sid", o.session_id}});
    if (o.status == "done") {
        if (!text.empty()) store_.conv_append(conv, {{"t", "msg"}, {"from", "agent:" + job.agent_id}, {"text", text}, {"run", job.id}});
        if (o.max_turns) say(conv, "⏸ " + name + " reached the step limit. Write \"continue\" to go on.");
        if (job.name_chat && !pr.title.empty()) store_.conv_append(conv, {{"t", "title"}, {"title", pr.title}});
        for (auto& b : pr.sends) handle_send(js, b, name);
    }

    // The asker wants an answer: hand it back as a report job (also for failures, so chains never hang).
    if (top.kind == "agent" && top.notify) {
        std::string report = o.status == "done" ? (text.empty() ? "(no answer)" : text) : "(" + name + " could not finish: " + (o.error.empty() ? o.status : o.error) + ")";
        Job rj;
        rj.kind = "report";
        rj.agent_id = top.id;
        rj.user_id = job.user_id;
        rj.user_role = job.user_role;
        rj.task_id = job.task_id;
        rj.project_id = job.project_id;
        rj.hops = job.hops;
        rj.stack.assign(job.stack.begin(), job.stack.end() - 1);
        rj.prompt = "Report from " + name + ":\n" + report;
        std::string err;
        if (!rj.stack.empty()) {
            if (enqueue_internal(rj, err, false).empty()) say(rj.stack.back().conv, "⚠ Could not hand the report to " + agent_name(store_, top.id) + ": " + err);
            else say(rj.stack.back().conv, "↩ " + name + " reported back to " + agent_name(store_, top.id) + ".");
        }
    } else if (top.kind == "agent" && job.stack.size() >= 2) {
        // Information only: the asker is not woken up, but the people following the work see how it ended
        // (agents sometimes use reply="no" by mistake; the work must not vanish without a trace).
        const std::string where = job.stack[job.stack.size() - 2].conv, asker = agent_name(store_, top.id);
        if (o.status == "done") say(where, "ℹ " + name + " finished (" + asker + " did not ask for an answer): " + truncate_utf8(text.empty() ? "(no answer)" : text, 400));
        else say(where, "ℹ " + name + " could not finish (" + asker + " did not ask for an answer): " + (o.error.empty() ? o.status : o.error));
    }
}

}  // namespace pc2
