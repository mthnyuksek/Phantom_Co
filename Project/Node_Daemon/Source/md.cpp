#include "md.hpp"

#include <algorithm>

namespace pc2 {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string MdDoc::get(const std::string& key, const std::string& def) const {
    for (auto& [k, v] : header)
        if (k == key) return v;
    return def;
}
std::vector<std::string> MdDoc::all(const std::string& key) const {
    std::vector<std::string> out;
    for (auto& [k, v] : header)
        if (k == key) out.push_back(v);
    return out;
}
void MdDoc::add(const std::string& key, const std::string& value) {
    std::string v = value;
    std::replace(v.begin(), v.end(), '\n', ' ');
    std::replace(v.begin(), v.end(), '\r', ' ');
    header.emplace_back(key, v);
}

MdDoc md_parse(const std::string& in) {
    MdDoc d;
    std::string t = in;
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
    size_t pos = 0;
    auto next_line = [&](std::string& line) {
        if (pos >= t.size()) return false;
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        line = t.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = e + 1;
        return true;
    };
    std::string line;
    size_t save = pos;
    if (next_line(line) && trim(line) == "---") {
        bool closed = false;
        std::vector<std::pair<std::string, std::string>> h;
        while (next_line(line)) {
            if (trim(line) == "---") { closed = true; break; }
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            h.emplace_back(trim(line.substr(0, c)), trim(line.substr(c + 1)));
        }
        if (closed) d.header = std::move(h);
        else pos = save;
    } else {
        pos = save;
    }
    d.body = pos < t.size() ? t.substr(pos) : "";
    while (!d.body.empty() && (d.body.front() == '\n' || d.body.front() == '\r')) d.body.erase(0, 1);
    while (!d.body.empty() && (d.body.back() == '\n' || d.body.back() == '\r' || d.body.back() == ' ')) d.body.pop_back();
    return d;
}

std::string md_serialize(const MdDoc& d) {
    std::string out = "---\n";
    for (auto& [k, v] : d.header) out += k + ": " + v + "\n";
    out += "---\n";
    if (!d.body.empty()) out += d.body + "\n";
    return out;
}

static std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        std::string p = trim(s.substr(a, b - a));
        if (!p.empty()) out.push_back(p);
        a = b + 1;
    }
    return out;
}

json md_to_json(const MdDoc& d, const std::vector<Field>& fields, const char* body_member) {
    json j = json::object();
    for (auto& f : fields) {
        switch (f.kind) {
            case 'S': j[f.js] = d.get(f.md, f.def); break;
            case 'B': { std::string v = d.get(f.md, f.def); j[f.js] = (v == "true" || v == "yes" || v == "1"); break; }
            case 'I': { std::string v = d.get(f.md, f.def); try { j[f.js] = std::stoi(v.empty() ? "0" : v); } catch (...) { j[f.js] = 0; } break; }
            case 'L': j[f.js] = d.all(f.md); break;
            case 'C': j[f.js] = split_commas(d.get(f.md, f.def)); break;
        }
    }
    if (body_member) j[body_member] = d.body;
    return j;
}

MdDoc json_to_md(const json& j, const std::vector<Field>& fields, const char* body_member) {
    MdDoc d;
    for (auto& f : fields) {
        if (!j.contains(f.js)) continue;
        const json& v = j[f.js];
        switch (f.kind) {
            case 'S': if (v.is_string() && !v.get<std::string>().empty()) d.add(f.md, v.get<std::string>()); break;
            case 'B': d.add(f.md, v.is_boolean() && v.get<bool>() ? "true" : "false"); break;
            case 'I': if (v.is_number()) d.add(f.md, std::to_string(v.get<long long>())); break;
            case 'L': if (v.is_array()) for (auto& x : v) if (x.is_string()) d.add(f.md, x.get<std::string>()); break;
            case 'C': {
                if (!v.is_array()) break;
                std::string s;
                for (auto& x : v) if (x.is_string()) s += (s.empty() ? "" : ", ") + x.get<std::string>();
                if (!s.empty()) d.add(f.md, s);
                break;
            }
        }
    }
    if (body_member && j.contains(body_member) && j[body_member].is_string()) d.body = j[body_member];
    return d;
}

}  // namespace pc2
