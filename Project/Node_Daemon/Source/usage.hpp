#pragma once
// How much of the Claude / Codex subscription limits is left (for script built-ins such as claude_left_5h).
// Claude prints a rate_limit_event in every stream-json run; Codex writes rate_limits into its session file.
// The newest values per sign-in ("claude", "claude#<account>", "codex", "codex#<account>") are kept in <data>\usage.json.
// When a value is too old, refresh() makes one tiny run (cheapest model, one short prompt) to read it again.
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "accounts.hpp"
#include "process.hpp"
#include "util.hpp"

namespace pc2 {

class Usage {
public:
    Usage(const fs::path& data_dir, Accounts* accounts);
    ~Usage();
    Usage(const Usage&) = delete;
    Usage& operator=(const Usage&) = delete;

    void record_claude(const std::string& brain, const json& rate_limit_info);
    void record_codex(const std::string& brain, const json& rate_limits);

    // {provider, left_5h, left_week, resets_5h, resets_week} (Claude) or {provider, left, window_minutes, resets_at, plan} (Codex),
    // plus "at" (unix seconds of the reading).
    std::optional<json> get(const std::string& brain);
    json all();
    bool fresh(const std::string& brain, long long max_age_s);

    // Starts one probe run in the background (nothing when one is already going). refreshing() = still going.
    void refresh(const std::string& brain);
    bool refreshing(const std::string& brain);
    void shutdown();

private:
    void probe(const std::string& brain);
    void save_locked();

    fs::path file_;
    Accounts* accounts_;
    std::mutex mu_;
    json data_;
    std::set<std::string> busy_;
    std::vector<std::thread> threads_;
    std::vector<Process*> procs_;
    bool stopping_ = false;
};

}  // namespace pc2
