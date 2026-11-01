#pragma once
#include <map>
#include <mutex>
#include <string>

#include "util.hpp"

namespace pc2 {

// Accounts, passwords, sessions and invites of this Node. Lives in a local
// file (never in the Vault) so agents that may edit Vault files can neither
// read password hashes nor change anybody's role.
//   roles: "owner" (everything), "editor" (works with agents), "viewer" (read only)
class Auth {
public:
    explicit Auth(fs::path file);

    bool has_users();
    int owner_count();
    bool exists(const std::string& id);
    std::string role(const std::string& id);  // "" when unknown
    void add_user(const std::string& id, const std::string& role);
    void set_role(const std::string& id, const std::string& role);
    void remove_user(const std::string& id);

    bool has_password(const std::string& id);
    void set_password(const std::string& id, const std::string& password);  // also ends the user's sessions
    bool verify(const std::string& id, const std::string& password);

    // Brute force protection per key (e.g. user name + address).
    bool login_allowed(const std::string& key);
    void login_failed(const std::string& key);
    void login_ok(const std::string& key);

    std::string new_session(const std::string& id);  // returns the secret token
    std::string session_user(const std::string& token);
    void end_session(const std::string& token);

    std::string new_invite(const std::string& id);  // returns the secret token
    std::string invite_user(const std::string& token);
    void use_invite(const std::string& token);

    // Token a friend's program uses to lend its Claude/Codex to this company (one per user, no expiry;
    // ended by a new token, a password change or removing the user).
    std::string new_worker_token(const std::string& id);
    std::string worker_user(const std::string& token);  // "" when unknown
    void end_worker(const std::string& id);

private:
    void load();
    void save();
    void purge();

    std::mutex mu_;
    fs::path file_;
    json data_;
    struct Throttle { int fails = 0; long long until = 0; };
    std::map<std::string, Throttle> throttle_;
};

bool valid_password(const std::string& pw);  // at least 8 characters

}  // namespace pc2
