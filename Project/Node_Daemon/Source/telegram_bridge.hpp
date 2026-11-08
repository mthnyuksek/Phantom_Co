#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "auth.hpp"
#include "mcp.hpp"
#include "runner.hpp"
#include "store.hpp"

namespace pc2 {

// Talking to ghosts from Telegram. When the Telegram MCP server (Settings > MCP) has "listen" switched on, this reads the
// bot's messages (getUpdates long polling) and turns commands into chats:
//   /agents (/phantoms)   list the ghosts           /<ghost> text   send a message to that ghost (it becomes the current one)
//   plain text            goes to the current ghost  /new           the current ghost forgets the chat (new session)
//   /yes, /no             answer the newest approval /link <code>   connect this Telegram account (code from the Profile)
// Only linked Telegram accounts may use it, and the ghosts work with the rights of the linked Phantom Co. person. Each
// (Telegram account, ghost) pair has its own normal chat ("Telegram · <ghost>"); answers, failures and approval requests
// written there go back to Telegram while the person's last message in it came from Telegram.
// State: <data>\telegram.json (offset, links, current ghost, chats). One bot can be read by one program only.
class TelegramBridge {
public:
    TelegramBridge(Store& store, Auth& auth, Runner& runner, Mcp& mcp, const fs::path& data_dir);
    void start();
    void stop();  // no join: a long poll may be waiting; the threads end on their own (the object is never deleted)

    void on_append(const std::string& conv, const json& line);  // Store hook: every conversation line
    json status();                                    // {state: off|on|error, text, bot}
    json link_info(const std::string& user_id);       // {on, bot, linked: [{name}]}
    std::string new_link_code(const std::string& user_id);
    void unlink(const std::string& user_id);
    json inbox();                                     // the last messages the bot got (for the read_messages tool)
    // Sends a notice to every Telegram account linked to this person (several threads may call it; no lock is held while sending).
    void send_to_user(const std::string& user_id, const std::string& en, const std::string& tr);
    // Ask blocks of scripts: /yes, /no and /answer <value> reach them through these (set once at start).
    void set_asks(std::function<json(const std::string& user, bool owner)> list,
                  std::function<bool(const json& ask, const std::string& user, bool owner, const std::string& by, bool approved, const std::string& value, std::string& err)> answer);

private:
    void poll_loop();
    void send_loop();
    void handle(const json& msg, const std::string& token);
    void reply(const std::string& chat, const std::string& text);
    void to_ghost(const std::string& tg, const std::string& chat, const json& agent, const std::string& text, const json& user, const std::string& role, bool tr);
    std::vector<std::pair<std::string, json>> commands();  // "/cmd" name -> agent, in company order
    bool config(std::string& token);                        // the listening Telegram server, if any
    void load();
    void save();  // caller holds mu_

    Store& store_;
    Auth& auth_;
    Runner& runner_;
    Mcp& mcp_;
    fs::path file_;
    std::mutex mu_;  // never held while calling Store / Runner / Mcp (Store calls on_append under its own lock)
    json state_ = json::object();
    std::map<std::string, std::pair<std::string, long long>> codes_;  // code -> {user, expiry (unix s)}
    std::deque<json> inbox_;
    std::string status_ = "off", status_text_, bot_;
    std::set<std::string> watched_;  // conversations whose answers go to Telegram

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<std::pair<std::string, json>> queue_;  // {conv, line} to forward
    std::atomic<bool> stop_{false};
    std::thread poll_, sender_;
    std::string token_now_;  // the token the sender uses
    std::function<json(const std::string&, bool)> asks_list_;
    std::function<bool(const json&, const std::string&, bool, const std::string&, bool, const std::string&, std::string&)> asks_answer_;
};

}  // namespace pc2
