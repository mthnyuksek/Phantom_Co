#pragma once
#include <filesystem>
#include <string>

#include "json.hpp"

namespace pc2 {

namespace fs = std::filesystem;
using json = nlohmann::json;

// UTF-8 <-> wide / path helpers (all strings in the program are UTF-8).
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);
fs::path P(const std::string& utf8);
std::string U8(const fs::path& p);

std::string new_id();   // ULID: 26 chars, time-sortable, collision-free across Nodes
std::string now_iso();  // UTC, e.g. 2026-11-01T12:00:00Z

bool read_file(const fs::path& p, std::string& out);
bool write_file_atomic(const fs::path& p, const std::string& data);
json read_json(const fs::path& p, const json& fallback = json::object());
bool write_json(const fs::path& p, const json& j);

// JSON dump that never throws on invalid UTF-8 (replaces bad bytes).
std::string dumps(const json& j, int indent = -1);

std::string slugify(const std::string& s, size_t max_len = 24);
bool valid_id(const std::string& s);                       // [A-Za-z0-9_-]{1,64}
std::string truncate_utf8(const std::string& s, size_t max_bytes);
std::string sha256_hex_of(const std::string& data);  // lower-case hex SHA-256

}  // namespace pc2
