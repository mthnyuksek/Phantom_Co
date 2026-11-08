#pragma once
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "store.hpp"
#include "util.hpp"

namespace pc2 {

// MCP servers: tools from outside (a browser, YouTube, Telegram, the owner's own servers) for the ghosts.
// Settings > MCP (owner, only at this PC). The list is <data>\mcp.json, outside the Vault on purpose: a server is a
// command that runs on this PC. Secret values (tokens, keys) are DPAPI-encrypted in <data>\secrets.json ("mcp").
// A skill opens servers by name (`mcp: youtube, browser`); agents act with the rights of the person who asked.
//
// A run never gets a server's command or secrets directly: every server is started as
//   phantom_co.exe --mcp-proxy <name> --mcp-port <port> --mcp-key <one-time key of the run>
// The proxy asks this program for the server (POST /api/mcp/session), then either starts the real server and passes
// its messages through, or serves a built-in one (telegram, youtube) itself. For a tool that needs approval, it asks
// (POST /api/mcp/ask) and waits until a person approves or declines the call in the chat.
struct McpRun {
    std::string run_id, agent_id, user_id;
    std::string conv;      // the conversation of the person the work is done for (approvals are asked there)
    std::string workdir;   // the run's working folder (files the built-in servers may send or upload)
    std::set<std::string> servers;
};

class Mcp {
public:
    Mcp(const fs::path& data_dir, Store& store);
    void set_port(int port) { port_ = port; }
    int port() const { return port_; }
    // Called when a tool call starts waiting for a yes/no, under this object's lock (user, conversation, ghost name, tool). Set once at start.
    void set_on_approval(std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> f) { on_approval_ = std::move(f); }

    // ---- the list (Settings > MCP)
    json list();                               // without secret values: secrets_set = names that have a value
    std::optional<json> get(const std::string& name);
    // Adds or changes a server. `secrets`: name -> value ("" leaves an existing value as it is). False + error.
    bool save(const json& in, const std::map<std::string, std::string>& secrets, bool is_new, std::string& error);
    bool remove(const std::string& name);
    bool refresh_folder(const std::string& name, std::string& error);  // copy the source folder again
    static json catalog();                     // ready-made servers: browser, youtube, telegram
    std::string secret(const std::string& name, const std::string& key);
    void set_secret(const std::string& name, const std::string& key, const std::string& value);
    fs::path data_dir() const { return dir_; }

    // ---- runs
    std::string open_run(const McpRun& r);      // returns the run's key
    void close_run(const std::string& key);     // pending approvals of the run are cancelled
    // What the proxy needs to start a server of this run: {kind, builtin, command, args, cwd, env, secrets, options, approve, approve_tools}
    std::optional<json> session(const std::string& key, const std::string& name, std::string& error);

    // ---- approvals
    // Asks for a tool call (writes the request into the chat) and waits up to `wait_s` seconds. `id` "" = a new request,
    // else keep waiting for that one. Returns {id, status: pending|approved|declined|cancelled, by, note}.
    json ask(const std::string& key, const std::string& server, const std::string& tool, const json& args, const std::string& id, int wait_s);
    json pending(const std::string& conv, const std::string& user_id, bool owner);  // [{id, can_decide, ...}] of one conversation ("" = all)
    bool decide(const std::string& id, const std::string& user_id, const std::string& user_name, bool owner, bool approve, std::string& error);

private:
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> on_approval_;
    struct Approval {
        std::string id, key, run_id, agent_id, user_id, conv, server, label, tool, args, status = "pending", by;
    };
    void load();
    void save_list();
    std::string label_of(const std::string& name);

    fs::path dir_;
    Store& store_;
    int port_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    json items_ = json::array();
    std::map<std::string, McpRun> runs_;          // key -> run
    std::map<std::string, Approval> approvals_;   // id -> approval (kept while the run lives)
};

// The proxy side: runs inside `phantom_co.exe --mcp-proxy` (stdin/stdout = the MCP connection to the CLI).
int mcp_proxy_main(const std::string& name, int port, const std::string& key);

// DPAPI helpers shared with Accounts (encrypted for this Windows user, base64).
std::string seal_secret(const std::string& plain, const wchar_t* what);
std::string open_secret(const std::string& sealed);

}  // namespace pc2
