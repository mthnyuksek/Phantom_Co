#include "auth.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>

namespace pc2 {

// ------------------------------------------------------------------- crypto
static std::string to_hex(const unsigned char* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
    return s;
}
static std::string from_hex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    return out;
}
static std::string random_hex(size_t n) {
    std::string b(n, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(b.data()), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return to_hex(reinterpret_cast<const unsigned char*>(b.data()), n);
}
static std::string sha256_hex(const std::string& s) {
    unsigned char out[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())), (ULONG)s.size(), out, 32);
    return to_hex(out, 32);
}
static std::string pbkdf2_hex(const std::string& pw, const std::string& salt_hex, int iter) {
    BCRYPT_ALG_HANDLE h = nullptr;
    unsigned char out[32] = {};
    std::string salt = from_hex(salt_hex);
    if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        BCryptDeriveKeyPBKDF2(h, reinterpret_cast<PUCHAR>(const_cast<char*>(pw.data())), (ULONG)pw.size(),
                              reinterpret_cast<PUCHAR>(salt.data()), (ULONG)salt.size(), (ULONGLONG)iter, out, 32, 0);
        BCryptCloseAlgorithmProvider(h, 0);
    }
    return to_hex(out, 32);
}
static bool ct_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char r = 0;
    for (size_t i = 0; i < a.size(); ++i) r |= (unsigned char)(a[i] ^ b[i]);
    return r == 0;
}
static long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool valid_password(const std::string& pw) { return pw.size() >= 8 && pw.size() <= 200; }

// --------------------------------------------------------------------- Auth
static constexpr int kIterations = 200000;

Auth::Auth(fs::path file) : file_(std::move(file)) { load(); }

void Auth::load() {
    data_ = read_json(file_, json::object());
    for (const char* k : {"users", "sessions", "invites"})
        if (!data_.contains(k) || !data_[k].is_object()) data_[k] = json::object();
    purge();
}
void Auth::save() { write_json(file_, data_); }
void Auth::purge() {
    long long t = now_s();
    for (const char* k : {"sessions", "invites"})
        for (auto it = data_[k].begin(); it != data_[k].end();) {
            if (it->value("exp", 0LL) < t) it = data_[k].erase(it);
            else ++it;
        }
}

bool Auth::has_users() { std::lock_guard<std::mutex> l(mu_); return !data_["users"].empty(); }
int Auth::owner_count() {
    std::lock_guard<std::mutex> l(mu_);
    int n = 0;
    for (auto& [id, u] : data_["users"].items()) if (u.value("role", "") == "owner") ++n;
    return n;
}
bool Auth::exists(const std::string& id) { std::lock_guard<std::mutex> l(mu_); return data_["users"].contains(id); }
std::string Auth::role(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    return data_["users"].contains(id) ? data_["users"][id].value("role", "") : "";
}
void Auth::add_user(const std::string& id, const std::string& role) {
    std::lock_guard<std::mutex> l(mu_);
    data_["users"][id] = {{"role", role}};
    save();
}
void Auth::set_role(const std::string& id, const std::string& role) {
    std::lock_guard<std::mutex> l(mu_);
    if (data_["users"].contains(id)) { data_["users"][id]["role"] = role; save(); }
}
void Auth::remove_user(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    data_["users"].erase(id);
    for (const char* k : {"sessions", "invites"})
        for (auto it = data_[k].begin(); it != data_[k].end();) {
            if (it->value("user", "") == id) it = data_[k].erase(it);
            else ++it;
        }
    save();
}
bool Auth::has_password(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    return data_["users"].contains(id) && data_["users"][id].contains("hash");
}
void Auth::set_password(const std::string& id, const std::string& pw) {
    std::lock_guard<std::mutex> l(mu_);
    if (!data_["users"].contains(id)) return;
    std::string salt = random_hex(16);
    data_["users"][id]["salt"] = salt;
    data_["users"][id]["iter"] = kIterations;
    data_["users"][id]["hash"] = pbkdf2_hex(pw, salt, kIterations);
    for (auto it = data_["sessions"].begin(); it != data_["sessions"].end();) {
        if (it->value("user", "") == id) it = data_["sessions"].erase(it);
        else ++it;
    }
    save();
}
bool Auth::verify(const std::string& id, const std::string& pw) {
    std::string salt, hash;
    int iter = kIterations;
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!data_["users"].contains(id) || !data_["users"][id].contains("hash")) return false;
        salt = data_["users"][id].value("salt", "");
        hash = data_["users"][id].value("hash", "");
        iter = data_["users"][id].value("iter", kIterations);
    }
    return ct_equal(pbkdf2_hex(pw, salt, iter), hash);
}

bool Auth::login_allowed(const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = throttle_.find(key);
    return it == throttle_.end() || it->second.until <= now_s();
}
void Auth::login_failed(const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    Throttle& t = throttle_[key];
    if (++t.fails >= 5) t.until = now_s() + std::min(600, 30 * (1 << std::min(t.fails - 5, 4)));
}
void Auth::login_ok(const std::string& key) {
    std::lock_guard<std::mutex> l(mu_);
    throttle_.erase(key);
}

std::string Auth::new_session(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    purge();
    std::string tok = random_hex(24);
    data_["sessions"][sha256_hex(tok)] = {{"user", id}, {"exp", now_s() + 30 * 86400}};
    save();
    return tok;
}
std::string Auth::session_user(const std::string& tok) {
    if (tok.size() != 48) return "";
    std::lock_guard<std::mutex> l(mu_);
    auto h = sha256_hex(tok);
    if (!data_["sessions"].contains(h)) return "";
    if (data_["sessions"][h].value("exp", 0LL) < now_s()) return "";
    std::string id = data_["sessions"][h].value("user", "");
    return data_["users"].contains(id) ? id : "";
}
void Auth::end_session(const std::string& tok) {
    std::lock_guard<std::mutex> l(mu_);
    data_["sessions"].erase(sha256_hex(tok));
    save();
}

std::string Auth::new_invite(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    purge();
    for (auto it = data_["invites"].begin(); it != data_["invites"].end();) {  // one open invite per user
        if (it->value("user", "") == id) it = data_["invites"].erase(it);
        else ++it;
    }
    std::string tok = random_hex(24);
    data_["invites"][sha256_hex(tok)] = {{"user", id}, {"exp", now_s() + 72 * 3600}};
    save();
    return tok;
}
std::string Auth::invite_user(const std::string& tok) {
    if (tok.size() != 48) return "";
    std::lock_guard<std::mutex> l(mu_);
    auto h = sha256_hex(tok);
    if (!data_["invites"].contains(h) || data_["invites"][h].value("exp", 0LL) < now_s()) return "";
    std::string id = data_["invites"][h].value("user", "");
    return data_["users"].contains(id) ? id : "";
}
void Auth::use_invite(const std::string& tok) {
    std::lock_guard<std::mutex> l(mu_);
    data_["invites"].erase(sha256_hex(tok));
    save();
}

}  // namespace pc2
