#include "notify.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace pc2 {

static constexpr size_t kMaxItems = 200;
static constexpr long long kOnceSeconds = 6 * 3600;

static json default_settings() {
    return {{"windows", true}, {"telegram", true}, {"failed", true}, {"ask", true}, {"budget", true}, {"done", true}, {"done_min", 5}};
}

Notifier::Notifier(const fs::path& data_dir) : file_(data_dir / L"notify.json"), settings_(default_settings()) {
    json j = read_json(file_);
    if (!j.is_object()) return;
    if (j.contains("settings") && j["settings"].is_object())
        for (auto& [k, v] : j["settings"].items())  // (a number read from a file is "unsigned", the default "integer": compare the kinds)
            if (settings_.contains(k) && v.is_boolean() == settings_[k].is_boolean() && v.is_number_integer() == settings_[k].is_number_integer()) settings_[k] = v;
    if (j.contains("items") && j["items"].is_array())
        for (auto& it : j["items"]) if (it.is_object() && items_.size() < kMaxItems) items_.push_back(it);
}

void Notifier::save() {
    json j = {{"settings", settings_}, {"items", json::array()}};
    for (auto& it : items_) j["items"].push_back(it);
    write_json(file_, j);
}

void Notifier::start() { worker_ = std::thread([this] { loop(); }); }

void Notifier::stop() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.detach();
}

void Notifier::set_toast(std::function<void(const Notice&)> f) { std::lock_guard<std::mutex> l(mu_); toast_ = std::move(f); }
void Notifier::set_telegram(std::function<void(const std::string&, const Notice&)> f) { std::lock_guard<std::mutex> l(mu_); telegram_ = std::move(f); }

void Notifier::post(Notice n) {
    if (stop_) return;
    std::lock_guard<std::mutex> l(mu_);
    if (!settings_.value(n.kind, true)) return;
    if (!n.once.empty()) {
        const long long now = (long long)std::time(nullptr);
        for (auto it = recent_.begin(); it != recent_.end();) it = now - it->second >= kOnceSeconds ? recent_.erase(it) : std::next(it);
        if (recent_.count(n.once)) return;
        recent_[n.once] = now;
    }
    items_.push_front({{"id", new_id()}, {"ts", now_iso()}, {"kind", n.kind}, {"user", n.user}, {"title", n.title}, {"text", n.text},
                       {"title_tr", n.title_tr.empty() ? n.title : n.title_tr}, {"text_tr", n.text_tr.empty() ? n.text : n.text_tr},
                       {"link", n.link}, {"read", false}});
    while (items_.size() > kMaxItems) items_.pop_back();
    save();
    queue_.push_back(std::move(n));
    cv_.notify_all();
}

void Notifier::loop() {
    while (!stop_) {
        Notice n;
        bool windows = false, telegram = false;
        std::function<void(const Notice&)> toast;
        std::function<void(const std::string&, const Notice&)> tg;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::seconds(2), [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            if (queue_.empty()) continue;
            n = std::move(queue_.front());
            queue_.pop_front();
            windows = settings_.value("windows", true);
            telegram = settings_.value("telegram", true);
            toast = toast_;
            tg = telegram_;
        }
        try {
            if (windows && toast) toast(n);
            if (telegram && tg) tg(n.user, n);
        } catch (const std::exception&) { /* a failed delivery must not stop the next one */ }
    }
}

static bool for_person(const json& it, const std::string& user, bool owner) {
    const std::string to = it.value("user", "");
    return to == user || (to.empty() && owner);
}

json Notifier::list(const std::string& user, bool owner) {
    std::lock_guard<std::mutex> l(mu_);
    json out = json::array();
    int unread = 0;
    for (auto& it : items_) {
        if (!for_person(it, user, owner)) continue;
        if (!it.value("read", false)) ++unread;
        if (out.size() < 50) out.push_back(it);
    }
    return {{"items", out}, {"unread", unread}};
}

int Notifier::unread(const std::string& user, bool owner) {
    std::lock_guard<std::mutex> l(mu_);
    int n = 0;
    for (auto& it : items_) if (for_person(it, user, owner) && !it.value("read", false)) ++n;
    return n;
}

void Notifier::mark_read(const std::string& user, bool owner, const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    bool changed = false;
    for (auto& it : items_) {
        if (!for_person(it, user, owner) || it.value("read", false) || (!id.empty() && it.value("id", "") != id)) continue;
        it["read"] = true;
        changed = true;
    }
    if (changed) save();
}

json Notifier::settings() {
    std::lock_guard<std::mutex> l(mu_);
    return settings_;
}

json Notifier::set_settings(const json& patch) {
    std::lock_guard<std::mutex> l(mu_);
    for (const char* k : {"windows", "telegram", "failed", "ask", "budget", "done"})
        if (patch.contains(k) && patch[k].is_boolean()) settings_[k] = patch[k];
    if (patch.contains("done_min") && patch["done_min"].is_number_integer()) settings_["done_min"] = std::clamp(patch["done_min"].get<int>(), 0, 1440);
    save();
    return settings_;
}

int Notifier::done_minutes() {
    std::lock_guard<std::mutex> l(mu_);
    return settings_.value("done", true) ? settings_.value("done_min", 5) : -1;
}

}  // namespace pc2
