#pragma once
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace pc2 {

// A Markdown file with a "---" header of `key: value` lines and a free text body
// (the Obsidian-like format every Vault entity uses). Keys may repeat.
struct MdDoc {
    std::vector<std::pair<std::string, std::string>> header;
    std::string body;

    std::string get(const std::string& key, const std::string& def = "") const;
    std::vector<std::string> all(const std::string& key) const;
    void add(const std::string& key, const std::string& value);
};

MdDoc md_parse(const std::string& text);
std::string md_serialize(const MdDoc& d);

// Field table: maps header keys <-> JSON members of an entity.
//   S string, B bool, I integer, L list written as repeated lines, C comma separated list
struct Field {
    const char* md;
    const char* js;
    char kind;
    const char* def;  // default when the key is missing (as text)
};

json md_to_json(const MdDoc& d, const std::vector<Field>& fields, const char* body_member);
MdDoc json_to_md(const json& j, const std::vector<Field>& fields, const char* body_member);

}  // namespace pc2
