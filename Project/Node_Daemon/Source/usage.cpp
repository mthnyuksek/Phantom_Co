#include "usage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>

#include "providers.hpp"
#include "store.hpp"

namespace pc2 {

namespace {

long long now_s() { return static_cast<long long>(std::time(nullptr)); }

double pct(double v) { return std::round(std::clamp(v, 0.0, 100.0) * 100.0) / 100.0; }  // 2 decimals, like script Numbers

double left_of_utilization(double u) { return pct(100.0 - u * 100.0); }  // Claude: 0..1 used -> percent left

bool probes_off() {  // tests never start a real CLI
    wchar_t buf[8];
    DWORD n = GetEnvironmentVariableW(L"PHANTOM_USAGE_PROBE", buf, 8);
    return n > 0 && n < 8 && std::wstring(buf) == L"off";
}

}  // namespace

Usage::Usage(const fs::path& data_dir, Accounts* accounts) : file_(data_dir / L"usage.json"), accounts_(accounts) {
    data_ = read_json(file_, json::object());
    if (!data_.is_object()) data_ = json::object();
}

Usage::~Usage() { shutdown(); }

void Usage::shutdown() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        stopping_ = true;
        for (Process* p : procs_) p->kill();
        threads.swap(threads_);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

void Usage::save_locked() { write_json(file_, data_); }

void Usage::record_claude(const std::string& brain, const json& info) {
    if (!info.is_object()) return;
    json w = info.value("unifiedWindows", json::object());
    if (!w.is_object()) return;
    json five = w.value("five_hour", json::object()), week = w.value("seven_day", json::object());
    if (!five.is_object() || !five.contains("utilization") || !five["utilization"].is_number()) return;
    json r = {{"provider", "claude"}, {"at", now_s()}, {"left_5h", left_of_utilization(five["utilization"].get<double>())},
              {"resets_5h", five.value("resetsAt", 0LL)}};
    if (week.is_object() && week.contains("utilization") && week["utilization"].is_number()) {
        r["left_week"] = left_of_utilization(week["utilization"].get<double>());
        r["resets_week"] = week.value("resetsAt", 0LL);
    }
    std::lock_guard<std::mutex> lk(mu_);
    data_[brain] = r;
    save_locked();
}

void Usage::record_codex(const std::string& brain, const json& rl) {
    if (!rl.is_object() || !rl.contains("primary") || !rl["primary"].is_object()) return;
    const json& p = rl["primary"];
    if (!p.contains("used_percent") || !p["used_percent"].is_number()) return;
    json r = {{"provider", "codex"}, {"at", now_s()}, {"left", pct(100.0 - p["used_percent"].get<double>())},
              {"window_minutes", p.value("window_minutes", 0LL)}, {"resets_at", p.value("resets_at", 0LL)}};
    if (rl.contains("plan_type") && rl["plan_type"].is_string()) r["plan"] = rl["plan_type"];
    std::lock_guard<std::mutex> lk(mu_);
    data_[brain] = r;
    save_locked();
}

std::optional<json> Usage::get(const std::string& brain) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!data_.contains(brain) || !data_[brain].is_object()) return std::nullopt;
    return data_[brain];
}

json Usage::all() {
    std::lock_guard<std::mutex> lk(mu_);
    return data_;
}

bool Usage::fresh(const std::string& brain, long long max_age_s) {
    auto r = get(brain);
    return r && now_s() - r->value("at", 0LL) <= max_age_s;
}

bool Usage::refreshing(const std::string& brain) {
    std::lock_guard<std::mutex> lk(mu_);
    return busy_.count(brain) > 0;
}

void Usage::refresh(const std::string& brain) {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopping_ || busy_.count(brain) || probes_off()) return;
    busy_.insert(brain);
    threads_.emplace_back([this, brain] {
        try { probe(brain); } catch (const std::exception&) {}
        std::lock_guard<std::mutex> l2(mu_);
        busy_.erase(brain);
    });
}

// One short run on that sign-in; its output carries the usage (Claude) or its session file does (Codex).
void Usage::probe(const std::string& brain) {
    std::string base = brain, account;
    size_t h = brain.find('#');
    if (h != std::string::npos) { base = brain.substr(0, h); account = brain.substr(h + 1); }
    if (base != "claude" && base != "codex") return;

    std::error_code ec;
    fs::path tmp = config_dir() / L"Tmp" / L"Usage_Probe";
    fs::create_directories(tmp, ec);
    RunSpec spec;
    spec.provider = base;
    spec.workdir = U8(tmp);
    spec.prompt = "Say only: ok";
    spec.max_turns = 1;
    spec.system_prompt = "Answer with one word.";
    fs::path sysfile = tmp / P(new_id() + ".system.md");
    if (base == "claude") {
        spec.model = "haiku";  // the cheapest model: the answer does not matter, only the usage event
        write_file_atomic(sysfile, spec.system_prompt);
        spec.system_file = U8(sysfile);
    }
    if (!account.empty() && accounts_) spec.env = accounts_->env(base, account);
    ProcessSpec ps;
    std::string err;
    if (!build_command(spec, ps, err)) return;
    Process proc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        if (!proc.start(ps, err)) { fs::remove(sysfile, ec); return; }
        procs_.push_back(&proc);
    }
    std::atomic<bool> done{false};
    std::thread watchdog([&] {  // a CLI that hangs must not keep a script block waiting for ever
        for (int i = 0; i < 900 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done) proc.kill();
    });
    LineParser parser(base);
    proc.read_lines([&](const std::string& line) { parser.feed(line); });
    proc.wait();
    done = true;
    watchdog.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        procs_.erase(std::remove(procs_.begin(), procs_.end(), &proc), procs_.end());
    }
    fs::remove(sysfile, ec);
    if (base == "claude") record_claude(brain, parser.rate_limits);
    else record_codex(brain, codex_rate_limits(parser.session_id, account.empty() || !accounts_ ? fs::path() : accounts_->home(account)));
}

}  // namespace pc2
