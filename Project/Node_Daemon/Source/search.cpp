#include "search.hpp"

#include <algorithm>

namespace pc2 {

namespace {

constexpr size_t kPerConversation = 3;
constexpr size_t kMaxFound = 2000;
constexpr size_t kContext = 70;

// Lower case without the Turkish marks; map[i] = where folded byte i starts in the original text.
std::string fold(const std::string& s, std::vector<size_t>* map) {
    std::string out;
    out.reserve(s.size());
    if (map) { map->clear(); map->reserve(s.size() + 1); }
    auto put = [&](char c, size_t at) { out += c; if (map) map->push_back(at); };
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { put((char)tolower(c), i); ++i; continue; }
        if (i + 1 < s.size()) {
            const unsigned two = (unsigned)c << 8 | (unsigned char)s[i + 1];
            char ascii = 0;
            switch (two) {
                case 0xC387: case 0xC3A7: ascii = 'c'; break;  // Ç ç
                case 0xC49E: case 0xC49F: ascii = 'g'; break;  // Ğ ğ
                case 0xC4B0: case 0xC4B1: ascii = 'i'; break;  // İ ı
                case 0xC396: case 0xC3B6: ascii = 'o'; break;  // Ö ö
                case 0xC59E: case 0xC59F: ascii = 's'; break;  // Ş ş
                case 0xC39C: case 0xC3BC: ascii = 'u'; break;  // Ü ü
                default: break;
            }
            if (ascii) { put(ascii, i); i += 2; continue; }
        }
        put((char)c, i);
        ++i;
    }
    if (map) map->push_back(s.size());
    return out;
}

// Moves an offset back to the start of a UTF-8 character.
size_t char_start(const std::string& s, size_t i) {
    while (i > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return i;
}

std::string one_line(std::string s) {
    for (char& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

struct Finder {
    std::string needle;
    bool find(const std::string& text, json& hit) const {
        if (text.size() < needle.size()) return false;
        std::vector<size_t> map;
        const std::string hay = fold(text, &map);
        const size_t at = hay.find(needle);
        if (at == std::string::npos) return false;
        const size_t a = map[at], b = map[at + needle.size()];
        const size_t from = char_start(text, a > kContext ? a - kContext : 0);
        const size_t to = char_start(text, std::min(text.size(), b + kContext));
        hit["before"] = (from > 0 ? "…" : "") + one_line(text.substr(from, a - from));
        hit["match"] = one_line(text.substr(a, b - a));
        hit["after"] = one_line(text.substr(b, to - b)) + (to < text.size() ? "…" : "");
        return true;
    }
};

}  // namespace

json search_vault(Store& store, const std::string& query, size_t limit, const std::function<bool(const json&)>& can_see) {
    Finder f;
    f.needle = fold(query, nullptr);
    std::vector<json> found;
    bool more = false;
    auto add = [&](json hit) {
        if (found.size() >= kMaxFound) { more = true; return; }
        found.push_back(std::move(hit));
    };

    std::map<std::string, bool> seen_conv;  // conversation id -> may read (runs ask again)
    std::map<std::string, std::string> task_of;
    for (const std::string& id : store.conv_ids()) {
        auto cv = store.conv_load(id);
        if (!cv) continue;
        const json& meta = (*cv)["meta"];
        const bool ok = can_see(meta);
        seen_conv[id] = ok;
        if (!ok) continue;
        const std::string kind = meta.value("kind", "");
        std::string link = "#/chats/" + id, type = kind == "agents" ? "agents" : "chat";
        if (kind == "task") { link = "#/tasks/" + meta.value("task_id", ""); type = "task_chat"; }
        else if (kind == "script") { link = "#/scripts/" + meta.value("script_id", "") + "/run/" + meta.value("script_run_id", ""); type = "script_chat"; }
        const std::string title = meta.value("title", "");
        size_t hits = 0;
        json hit;
        if (f.find(title, hit)) {
            hit.update({{"type", type}, {"title", title}, {"link", link}, {"ts", meta.value("created", "")}});
            add(hit);
            ++hits;
        }
        const json& msgs = (*cv)["messages"];
        for (auto it = msgs.rbegin(); it != msgs.rend() && hits < kPerConversation; ++it) {  // the newest lines first
            json h;
            if (!f.find(it->value("text", ""), h)) continue;
            h.update({{"type", type}, {"title", title}, {"link", link}, {"ts", it->value("ts", "")}});
            add(h);
            ++hits;
        }
    }
    for (auto& t : store.tasks()) {
        const std::string title = t.value("title", "");
        json hit;
        if (f.find(title, hit) || f.find(t.value("description", ""), hit)) {
            hit.update({{"type", "task"}, {"title", title}, {"link", "#/tasks/" + t.value("id", "")}, {"ts", t.value("updated", t.value("created", ""))}});
            add(hit);
        }
    }
    for (auto& p : store.projects()) {
        const std::string pname = p.value("name", ""), link = "#/projects/" + p.value("id", "");
        json hit;
        if (f.find(pname, hit) || f.find(p.value("summary", ""), hit)) {
            hit.update({{"type", "project"}, {"title", pname}, {"link", link}, {"ts", p.value("created", "")}});
            add(hit);
        }
        for (auto& n : p.value("notes", json::array())) {
            json h;
            if (f.find(n.value("name", ""), h) || f.find(n.value("body", ""), h)) {
                h.update({{"type", "note"}, {"title", pname + " · " + n.value("name", "")}, {"link", link}, {"ts", n.value("updated", p.value("created", ""))}});
                add(h);
            }
        }
    }
    for (auto& r : store.runs()) {
        const std::string err = r.value("error", ""), conv = r.value("conversation_id", "");
        json hit;
        if (err.empty() || !f.find(err, hit)) continue;
        auto it = seen_conv.find(conv);
        if (it == seen_conv.end() || !it->second) continue;
        auto agent = store.agent(r.value("agent_id", ""));
        hit.update({{"type", "run"}, {"title", agent ? agent->value("name", "") : r.value("agent_id", "")}, {"link", "#/runs/" + r.value("id", "")}, {"ts", r.value("created", "")}});
        add(hit);
    }
    std::stable_sort(found.begin(), found.end(), [](const json& a, const json& b) { return a.value("ts", "") > b.value("ts", ""); });
    if (found.size() > limit) { more = true; found.resize(limit); }
    return {{"results", found}, {"truncated", more}};
}

}  // namespace pc2
