#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "util.hpp"

namespace pc2 {

// One run that a friend's program (a "worker") executes with its own Claude/Codex.
struct RemoteJob {
    std::string run_id;
    std::string worker;  // user id of the friend
    std::string scope;   // "shared" | "project:<id>": the folder the run works in (mirrored on the worker)
    json payload;        // what the worker needs to start the CLI
    std::function<void(json)> on_event;

    std::mutex mu;
    std::condition_variable cv;
    bool taken = false;
    bool finished = false;
    std::atomic<bool> cancel{false};
    long long submitted = 0, last_seen = 0;  // unix seconds
    json result;                             // set by the worker when finished
};

// Host side of the "lend my Claude/Codex" feature. The worker polls (it is the one that opens the
// connection, so no port has to be opened on the friend's PC), takes a job, streams events back
// and uploads the result. A worker is "online" while it keeps polling.
class WorkerHub {
public:
    // ---- called by the runner
    // Hands a job to the worker; wait() blocks until the worker is done (or gave up).
    std::shared_ptr<RemoteJob> submit(const std::string& worker, const std::string& run_id, const std::string& scope,
                                      json payload, std::function<void(json)> on_event);
    // Returns false and sets `error` when the run could not be completed remotely.
    bool wait(const std::shared_ptr<RemoteJob>& job, std::atomic<bool>& cancel, int timeout_min, std::string& error);

    // ---- called by the server (the worker's requests)
    // Waits up to `seconds` for a job; nullptr when there is none.
    std::shared_ptr<RemoteJob> poll(const std::string& worker, const std::set<std::string>& providers, int seconds);
    std::shared_ptr<RemoteJob> find(const std::string& run_id, const std::string& worker);
    void touch(const std::shared_ptr<RemoteJob>& job);
    void finish(const std::shared_ptr<RemoteJob>& job, json result);

    bool online(const std::string& worker);
    std::set<std::string> providers(const std::string& worker);  // empty when offline
    json status();                                                 // [{user_id, providers, online}]
    void shutdown();

private:
    struct Worker {
        std::set<std::string> providers;
        long long last_poll = 0;
        std::deque<std::shared_ptr<RemoteJob>> queue;
    };
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Worker> workers_;
    std::map<std::string, std::shared_ptr<RemoteJob>> jobs_;  // run id -> job (while it lives)
    std::atomic<bool> stopping_{false};
};

}  // namespace pc2
