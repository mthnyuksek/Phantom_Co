#pragma once
#include <functional>

#include "auth.hpp"
#include "backup.hpp"
#include "runner.hpp"
#include "script_runner.hpp"
#include "store.hpp"
#include "telegram_bridge.hpp"
#include "tunnel.hpp"

namespace pc2 {

struct ServerOptions {
    fs::path ui_dir;  // when set, the UI is served from disk (live editing) instead of the embedded copy
    Config* config = nullptr;
    TelegramBridge* telegram = nullptr;  // talking to ghosts from Telegram; may be null
    ScriptRunner* scripts = nullptr;  // visual scripts (runs); may be null
    Usage* usage = nullptr;  // what is left of the Claude/Codex subscription limits; may be null
    Notifier* notifier = nullptr;  // the bell, Windows balloons, Telegram notices; may be null
    Backup* backup = nullptr;  // Vault backups (Settings > Backup); may be null
    Tunnel* tunnel = nullptr;  // "Internet access" (Cloudflare quick tunnel); may be null
    fs::path fresh_vault;  // the Vault folder this start created (it did not exist); removed when the first owner picks another folder
    std::function<void(bool)> on_listening;  // called once the port is bound (true) or could not be bound (false)
};

// Web server: REST API + the browser UI. Listens on 127.0.0.1 only unless the owner
// switched on network sharing. Every API call except sign-in needs a session.
// Returns 0 on a normal stop, 2 when the port is in use. Sets `restart` when the
// listening address changed and the caller should call run_server again.
int run_server(Store& store, Auth& auth, Runner& runner, const ServerOptions& opt, bool& restart);
void stop_server();

}  // namespace pc2
