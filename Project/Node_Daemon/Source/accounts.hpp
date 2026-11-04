#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace pc2 {

// Extra Claude Code / Codex sign-ins of this PC ("accounts"), next to "Auto" (whatever the CLI uses by itself).
// Each account has its own folder <data>\Accounts\<id>\ that the CLI uses as its home (CLAUDE_CONFIG_DIR / CODEX_HOME):
// the CLI keeps the sign-in there, Phantom Co. never copies tokens. The list is <data>\accounts.json, outside the
// Vault on purpose (agents with file tools must not reach sign-ins). An agent uses one with provider "claude#<id>".
class Accounts {
public:
    explicit Accounts(const fs::path& data_dir);

    json list();                                   // [{id, provider, name, created}]
    std::optional<json> get(const std::string& id);
    json add(const std::string& provider, const std::string& name);
    bool rename(const std::string& id, const std::string& name);
    bool remove(const std::string& id);            // also deletes the account folder (the sign-in)
    fs::path home(const std::string& id) const;    // <data>\Accounts\<id>

    // Environment changes for a run with this account ({} for Auto = "" id).
    std::vector<std::pair<std::string, std::string>> env(const std::string& provider, const std::string& id) const;

    // Sign-in state of an account ("" = Auto): {signed_in, email, plan, error}. Runs the CLI (a few seconds).
    json status(const std::string& provider, const std::string& id);

    // Opens a console window with the CLI's own sign-in for this account ("" = Auto). False + error when it cannot.
    bool open_login(const std::string& provider, const std::string& id, std::string& error);

private:
    void save();
    fs::path dir_;
    std::mutex mu_;
    json items_ = json::array();
};

}  // namespace pc2
