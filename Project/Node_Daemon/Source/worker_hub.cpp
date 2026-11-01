#include "worker_hub.hpp"

#include <chrono>

namespace pc2 {

using namespace std::chrono_literals;

static long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static constexpr long long kOnlineWindow = 45;   // seconds since the last poll
static constexpr long long kSilenceLimit = 120;  // a running job that sends nothing for this long is given up
static constexpr long long kPickupLimit = 90;    // a job nobody picks up for this long fails

std::shared_ptr<RemoteJob> WorkerHub::submit(const std::string& worker, const std::string& run_id, const std::string& scope,
                                             json payload, std::function<void(json)> on_event) {
    auto job = std::make_shared<RemoteJob>();
    job->run_id = run_id;
    job->worker = worker;
    job->scope = scope;
    job->payload = std::move(payload);
    job->on_event = std::move(on_event);
    job->submitted = job->last_seen = now_s();
    {
        std::lock_guard<std::mutex> lk(mu_);
        workers_[worker].queue.push_back(job);
        jobs_[run_id] = job;
    }
    cv_.notify_all();
    return job;
}

bool WorkerHub::wait(const std::shared_ptr<RemoteJob>& job, std::atomic<bool>& cancel, int timeout_min, std::string& error) {
    const long long started = now_s();
    bool cancel_sent = false;
    long long cancel_at = 0;
    std::unique_lock<std::mutex> lk(job->mu);
    while (!job->finished) {
        job->cv.wait_for(lk, 500ms);
        if (job->finished) break;
        if (stopping_) { error = "stopped"; break; }
        long long t = now_s();
        if (cancel && !cancel_sent) { job->cancel = true; cancel_sent = true; cancel_at = t; }
        if (cancel_sent && t - cancel_at > 15) { error = "stopped"; break; }  // the worker did not confirm in time
        if (!job->taken && t - job->submitted > kPickupLimit) {
            error = "The PC that should run this is not online. Ask your friend to switch \"lend my AI\" on.";
            break;
        }
        if (job->taken && t - job->last_seen > kSilenceLimit) { error = "The friend's PC stopped answering."; break; }
        if (job->taken && t - started > (long long)timeout_min * 60 + 30) { error = "Stopped after the " + std::to_string(timeout_min) + " minute limit."; break; }
    }
    bool ok = job->finished;
    lk.unlock();
    {
        std::lock_guard<std::mutex> g(mu_);
        auto& q = workers_[job->worker].queue;
        for (auto it = q.begin(); it != q.end();) it = (*it == job) ? q.erase(it) : std::next(it);  // never picked up
        jobs_.erase(job->run_id);
    }
    return ok;
}

std::shared_ptr<RemoteJob> WorkerHub::poll(const std::string& worker, const std::set<std::string>& providers, int seconds) {
    std::unique_lock<std::mutex> lk(mu_);
    Worker& w = workers_[worker];
    w.providers = providers;
    w.last_poll = now_s();
    auto take = [&]() -> std::shared_ptr<RemoteJob> {
        Worker& x = workers_[worker];
        for (auto it = x.queue.begin(); it != x.queue.end(); ++it) {
            auto job = *it;
            std::string prov = job->payload.value("provider", "");
            if (!providers.count(prov)) continue;
            x.queue.erase(it);
            std::lock_guard<std::mutex> jl(job->mu);
            job->taken = true;
            job->last_seen = now_s();
            return job;
        }
        return nullptr;
    };
    std::shared_ptr<RemoteJob> job = take();
    if (!job) {
        cv_.wait_for(lk, std::chrono::seconds(seconds), [&] { return stopping_ || [&] {
            for (auto& j : workers_[worker].queue) if (providers.count(j->payload.value("provider", ""))) return true;
            return false; }(); });
        if (!stopping_) job = take();
    }
    workers_[worker].last_poll = now_s();
    return job;
}

std::shared_ptr<RemoteJob> WorkerHub::find(const std::string& run_id, const std::string& worker) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(run_id);
    return it != jobs_.end() && it->second->worker == worker ? it->second : nullptr;
}

void WorkerHub::touch(const std::shared_ptr<RemoteJob>& job) {
    std::lock_guard<std::mutex> lk(job->mu);
    job->last_seen = now_s();
}

void WorkerHub::finish(const std::shared_ptr<RemoteJob>& job, json result) {
    {
        std::lock_guard<std::mutex> lk(job->mu);
        job->result = std::move(result);
        job->finished = true;
    }
    job->cv.notify_all();
}

bool WorkerHub::online(const std::string& worker) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = workers_.find(worker);
    return it != workers_.end() && now_s() - it->second.last_poll <= kOnlineWindow;
}

std::set<std::string> WorkerHub::providers(const std::string& worker) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = workers_.find(worker);
    return it != workers_.end() && now_s() - it->second.last_poll <= kOnlineWindow ? it->second.providers : std::set<std::string>{};
}

json WorkerHub::status() {
    std::lock_guard<std::mutex> lk(mu_);
    json out = json::array();
    for (auto& [id, w] : workers_) {
        bool on = now_s() - w.last_poll <= kOnlineWindow;
        out.push_back({{"user_id", id}, {"online", on}, {"providers", on ? json(w.providers) : json::array()}});
    }
    return out;
}

void WorkerHub::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
        for (auto& [id, j] : jobs_) j->cancel = true;
    }
    cv_.notify_all();
}

}  // namespace pc2
