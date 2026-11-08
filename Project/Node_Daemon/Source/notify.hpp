#pragma once
// Notifications: "something needs you or went wrong while you were away". Every notice goes to the bell in the sidebar of
// the person it is for and, depending on Settings, to a Windows balloon and to their linked Telegram account.
// Kept in <data>\notify.json (settings + the last 200 notices). Delivery runs on its own thread: post() only takes this
// object's lock, so it is safe to call under the locks of the Store, Mcp and ScriptRunner.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "util.hpp"

namespace pc2 {

struct Notice {
    std::string kind;   // failed | ask | budget | done (the switches of Settings > Notifications)
    std::string user;   // who it is for; "" = every owner
    std::string title, text, title_tr, text_tr;
    std::string link;   // a #/... address inside the app
    std::string once;   // not empty: the same key again within 6 hours is dropped
};

class Notifier {
public:
    explicit Notifier(const fs::path& data_dir);
    void start();
    void stop();  // no join: a Telegram send may be waiting; the object is never deleted

    void set_toast(std::function<void(const Notice& n)> f);                                    // set once at start
    void set_telegram(std::function<void(const std::string& user, const Notice& n)> f);        // set once at start

    void post(Notice n);
    json list(const std::string& user, bool owner);  // {items: newest first, unread}
    int unread(const std::string& user, bool owner);
    void mark_read(const std::string& user, bool owner, const std::string& id);  // "" = all of them
    json settings();
    json set_settings(const json& patch);
    int done_minutes();  // a chat or script that took at least this long says so when it ends

private:
    void loop();
    void save();  // caller holds mu_

    fs::path file_;
    std::mutex mu_;
    std::condition_variable cv_;
    json settings_;
    std::deque<json> items_;      // newest first
    std::deque<Notice> queue_;    // waiting for delivery
    std::map<std::string, long long> recent_;  // once key -> unix seconds
    std::function<void(const Notice&)> toast_;
    std::function<void(const std::string&, const Notice&)> telegram_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace pc2
