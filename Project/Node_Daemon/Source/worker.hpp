#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "util.hpp"

namespace pc2 {

// The friend's side of "lend my Claude/Codex": connects to a host company, takes jobs, runs the CLI of this PC
// inside a mirror of the run's folder and sends events, files and the result back.
//
// Safety: the host decides the prompt, so this side only ever starts `claude` / `codex` with file and web tools
// (no shell), inside its own mirror folder under %LOCALAPPDATA%\Phantom_Co\Worker. Nothing else on this PC is exposed.
class WorkerClient {
public:
    WorkerClient() = default;
    ~WorkerClient() { stop(); }
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    // `base_url` is the origin of the host, e.g. http://192.168.1.20:7777
    bool start(const std::string& base_url, const std::string& token, std::string& error);
    void stop();
    bool running() const { return running_; }
    json info();  // {running, url, status, providers}

private:
    void loop();
    void run_job(const json& job);
    void set_status(const std::string& s);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<void*> current_proc_{nullptr};
    std::mutex mu_;
    std::string url_, token_, status_ = "off";
    json providers_ = json::array();
};

}  // namespace pc2
