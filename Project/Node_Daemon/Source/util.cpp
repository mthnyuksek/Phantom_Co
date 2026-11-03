#include "util.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <mutex>
#include <random>

namespace pc2 {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path P(const std::string& utf8) { return fs::path(Widen(utf8)); }
std::string U8(const fs::path& p) { return Narrow(p.wstring()); }

std::string new_id() {
    static const char* A = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    std::string out(26, '0');
    for (int i = 9; i >= 0; --i) {
        out[i] = A[ms & 31];
        ms >>= 5;
    }
    std::lock_guard<std::mutex> lk(m);
    for (int i = 10; i < 26; ++i) out[i] = A[rng() & 31];
    return out;
}

std::string now_iso() {
    auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file_atomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

std::string dumps(const json& j, int indent) {
    return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

json read_json(const fs::path& p, const json& fallback) {
    std::string s;
    if (!read_file(p, s)) return fallback;
    json j = json::parse(s, nullptr, false);
    return j.is_discarded() ? fallback : j;
}

bool write_json(const fs::path& p, const json& j) { return write_file_atomic(p, dumps(j, 2) + "\n"); }

std::string slugify(const std::string& s, size_t max_len) {
    std::string out;
    bool dash = false;
    for (unsigned char c : s) {
        if (std::isalnum(c) && c < 128) {
            out += (char)std::tolower(c);
            dash = false;
        } else if (!dash && !out.empty()) {
            out += '_';
            dash = true;
        }
        if (out.size() >= max_len) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "ghost" : out;
}

bool valid_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) && c < 128) && c != '_' && c != '-') return false;
    return true;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

std::string sha256_hex_of(const std::string& data) {
    unsigned char out[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), (ULONG)data.size(), out, 32);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char b : out) { s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}

}  // namespace pc2
