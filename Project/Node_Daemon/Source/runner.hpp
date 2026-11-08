#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "accounts.hpp"
#include "mcp.hpp"
#include "notify.hpp"
#include "process.hpp"
#include "prompt.hpp"
#include "providers.hpp"
#include "store.hpp"
#include "usage.hpp"
#include "worker_hub.hpp"

namespace pc2 {

// One level of the reply stack: somebody (a user or an agent) who is waiting for an answer
// written into `conv`. Example: user asks A -> A's job has [user@C1]; A asks B -> B's job
// has [user@C1, A@X] (X = the A<->B conversation). B's answer goes into X and, because A
// wants a report, comes back to A as a "report" job that works in C1 again.
struct Frame {
    std::string kind;    // "user" | "agent" | "script" (id = "<script run id>/<block id>", see ScriptRunner)
    std::string id;
    std::string conv;
    bool notify = true;  // agent frames: send the answer back as a report job
};

struct Job {
    std::string id;       // run id (set by the Runner)
    std::string kind;     // chat | task | msg | report | script
    std::string agent_id;
    std::string user_id;  // the person this work is done for
    std::string user_role;
    std::string task_id;
    std::string project_id;
    std::string prompt;
    std::vector<Frame> stack;  // never empty
    int hops = 0;
    bool name_chat = false;    // ask the agent to title a new conversation
    const std::string& conv() const { return stack.back().conv; }
};

struct JobState {
    Job job;
    std::mutex mu;
    std::string status = "queued";  // queued | running | done | failed | cancelled | timeout
    std::string started;
    std::vector<json> events;
    std::ofstream events_file;
    std::deque<std::string> activity;  // last short lines, for the live view
    bool slow = false;                 // OpenCode said nothing for a while (a busy free service; OpenCode retries quietly)
    std::string brain;  // the agent's provider when queued: "claude", "claude#<account>", "claude@<friend>" (= its lane)
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    Process* proc = nullptr;
    std::string answer, answer_error;  // set by route(): the answer without <send>/<title>, or why it failed
    bool can_script = false;           // the agent has the Visual scripts skill in this run (its <script> blocks are saved)
    bool can_remember = false;         // the ghost keeps notes and this run saw them (its <remember> / <forget> blocks are applied)
};

// Called after every job has finished (no Runner lock held): status = done | failed | cancelled | timeout.
using JobFinishHook = std::function<void(const Job& job, const std::string& status, const std::string& answer, const std::string& error)>;

// Runs jobs with the provider CLIs, stores answers in conversations and passes work
// between agents. At most `max_concurrent` runs per provider; an agent never runs twice at once.
class Runner {
public:
    explicit Runner(Store& store);
    ~Runner();

    // Returns the run id, or "" and `error` (queue limit, unknown agent...).
    std::string enqueue(Job job, std::string& error);
    // Work that no person typed (script blocks): no per-person queue limit, no "conversation busy" refusal.
    std::string enqueue_routed(Job job, std::string& error) { return enqueue_internal(std::move(job), error, false); }
    void set_finish_hook(JobFinishHook f) { finish_hook_ = std::move(f); }  // set once at start
    void set_usage(Usage* u) { usage_ = u; }  // where local Claude/Codex runs leave the subscription usage they saw
    void set_notifier(Notifier* n) { notifier_ = n; }  // tells people that a budget ran out

    bool cancel_run(const std::string& run_id);
    int cancel_conversation(const std::string& conv_id);

    json events(const std::string& run_id, size_t after);        // {events, status, next}
    json active_jobs();                                         // for the UI
    json agent_activity();                                      // agent_id -> {state, conversation_id, task_id}
    bool conversation_busy(const std::string& conv_id);
    json conversation_live(const std::string& conv_id);         // [{agent_id, run_id, status, activity}]
    std::string task_state(const std::string& conv_id);         // "running" | "queued" | ""
    bool any_active();
    void shutdown();
    WorkerHub& hub() { return hub_; }  // friends' PCs that lend their Claude/Codex
    void set_accounts(Accounts* a) { accounts_ = a; }  // extra sign-ins of this PC ("claude#<id>")
    Accounts* accounts() { return accounts_; }
    void set_mcp(Mcp* m) { mcp_ = m; }  // MCP servers the skills may open (Settings > MCP)
    Mcp* mcp() { return mcp_; }
    bool brain_busy(const std::string& brain);  // a queued or running job uses this provider string

private:
    struct Lane {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::shared_ptr<JobState>> queue;
        std::set<std::string> running_agents;
        int running = 0;
        int cap = 0;  // 0 = the max_concurrent setting; remote lanes run one job at a time
        std::vector<std::thread> workers;
    };
    struct Outcome {
        std::string status = "done";  // done | failed | cancelled | timeout
        std::string text, error, session_id;
        int exit_code = 0;
        long long tokens_in = 0, tokens_out = 0, tokens_cached = 0;
        double cost = -1;
        bool max_turns = false;
    };

    std::string enqueue_internal(Job job, std::string& error, bool user_started);
    std::shared_ptr<JobState> find(const std::string& run_id);
    Lane* lane_for(const std::string& provider);  // creates the lane of a remote provider ("claude@<user>") or an account ("claude#<id>") on demand
    Outcome run_remote(const std::shared_ptr<JobState>& js, const RunSpec& spec, const std::string& worker, const std::string& scope, int timeout_min);
    void worker_loop(Lane& lane);
    std::deque<std::shared_ptr<JobState>>::iterator pick(Lane& lane);
    void execute(const std::shared_ptr<JobState>& js);
    Outcome run_demo(const std::shared_ptr<JobState>& js, const AgentContext& ctx, const std::string& workdir, const std::string& system_prompt);
    void route(const std::shared_ptr<JobState>& js, const Outcome& o);
    void handle_send(const std::shared_ptr<JobState>& js, const SendBlock& blk, const std::string& sender_name);
    void handle_script(const std::shared_ptr<JobState>& js, const ScriptBlock& blk, const std::string& sender_name);
    bool has_pending_request(const std::string& sender, const std::string& target);
    void emit(JobState& js, json ev);
    void say(const std::string& conv, const std::string& text);  // system line in a conversation
    bool over_budget(const Job& job, std::string& error);        // a limit of Overview is used up: tells the owners once per period
    void apply_memory(const std::shared_ptr<JobState>& js, const ParsedReply& pr);

    Store& store_;
    std::mutex reg_mu_;
    std::map<std::string, std::shared_ptr<JobState>> registry_;
    std::mutex lanes_mu_;
    std::map<std::string, std::unique_ptr<Lane>> lanes_;
    WorkerHub hub_;
    Accounts* accounts_ = nullptr;
    Mcp* mcp_ = nullptr;
    JobFinishHook finish_hook_;
    Usage* usage_ = nullptr;
    Notifier* notifier_ = nullptr;
    std::atomic<bool> stopping_{false};
};

}  // namespace pc2
