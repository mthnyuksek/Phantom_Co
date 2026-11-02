#include "prompt.hpp"

#include <algorithm>
#include <set>

namespace pc2 {

int role_rank(const std::string& r) { return r == "viewer" ? 0 : r == "editor" ? 1 : r == "owner" ? 2 : -1; }

bool AgentContext::has_tool(const std::string& t) const { return std::find(tools.begin(), tools.end(), t) != tools.end(); }
bool AgentContext::can_write() const { return has_tool("Write") || has_tool("Edit") || has_tool("Bash") || has_tool("PowerShell"); }

bool skill_fits_project(const json& skill, const std::string& project_id) {
    const json& ps = skill.contains("projects") && skill["projects"].is_array() ? skill["projects"] : json::array();
    if (ps.empty()) return true;  // no project list = every project and the shared files
    for (auto& p : ps) if (p.is_string() && p.get<std::string>() == project_id) return true;
    return false;
}

AgentContext resolve_agent(Store& store, const json& agent, const std::string& user_role, const std::string& project_id) {
    AgentContext c;
    c.agent = agent;
    static const std::set<std::string> kDirs = {"Agents", "Skills", "Rules", "Projects"};
    std::set<std::string> tools, dirs;
    int rank = role_rank(user_role);
    for (auto& id : agent.value("skills", json::array())) {
        auto s = store.skill(id.get<std::string>());
        if (!s || !s->value("enabled", true)) continue;
        if (role_rank(s->value("min_role", "editor")) > rank) continue;
        if (!skill_fits_project(*s, project_id)) continue;  // project skills stay out of other work
        for (auto& t : (*s)["tools"]) tools.insert(t.get<std::string>());
        for (auto& d : (*s)["dirs"]) if (kDirs.count(d.get<std::string>())) dirs.insert(d.get<std::string>());
        c.skills.push_back(*s);
    }
    for (const char* order : {"Read", "Glob", "Grep", "Write", "Edit", "Bash", "PowerShell", "WebSearch", "WebFetch"})
        if (tools.erase(order)) c.tools.push_back(order);
    for (auto& t : tools) c.tools.push_back(t);  // anything else, as written in the skill
    for (auto& d : dirs) c.add_dirs.push_back(U8(store.root() / P(d)));
    return c;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string build_system_prompt(Store& store, const PromptInput& in) {
    const json& a = in.ctx->agent;
    json company = store.company();
    std::string p = "You are " + a.value("name", "an agent") + ", " + (a.value("role", "").empty() ? "an agent" : a.value("role", "")) +
                    " at the company \"" + company.value("name", "Phantom Co.") + "\". Answer in the language the person writes in.\n";
    if (!company.value("goal", "").empty()) p += "Company goal: " + company.value("goal", "") + "\n";
    if (!in.user_name.empty())
        p += in.shared_chat ? "Several people may write in this chat; their messages start with their name.\n"
                            : "You are working for " + in.user_name + ".\n";

    p += "\n## Where you are\nPhantom Co. is a small app where people chat with agents and agents hand work to each other. Your working folder is " +
         in.workdir_label + ": " + in.workdir + ". Save files you create there.";
    if (!in.ctx->add_dirs.empty()) {
        p += " The vault folder is " + U8(store.root()) + "; your skills may edit these parts of it:";
        for (auto& d : in.ctx->add_dirs) p += " " + d + ";";
    }
    p += "\n";

    std::string rules;
    for (auto& r : store.rules())
        if (r.value("enabled", true) && !trim(r.value("body", "")).empty()) rules += "- " + trim(r.value("body", "")) + "\n";
    if (!rules.empty()) p += "\n## Company rules\n" + rules;

    if (!trim(a.value("persona", "")).empty()) p += "\n## Persona\n" + trim(a.value("persona", "")) + "\n";

    if (!in.ctx->skills.empty()) {
        p += "\n## Skills\n";
        for (auto& s : in.ctx->skills)
            if (!trim(s.value("body", "")).empty()) p += "### " + s.value("name", "") + "\n" + trim(s.value("body", "")) + "\n";
    }

    if (in.project.is_object() && !in.project.is_null()) {
        p += "\n## Project: " + in.project.value("name", "") + "\n";
        if (!trim(in.project.value("summary", "")).empty()) p += trim(in.project.value("summary", "")) + "\n";
        for (auto& n : in.project.value("notes", json::array()))
            if (n.value("enabled", true) && !trim(n.value("body", "")).empty()) p += "- " + n.value("name", "Note") + ": " + trim(n.value("body", "")) + "\n";
    }
    // What each agent does in this project (set on the project page); the agent's own part comes in full.
    const json roles = in.project.is_object() && in.project.contains("roles") && in.project["roles"].is_object() ? in.project["roles"] : json::object();
    auto role_of = [&](const json& id) { return roles.contains(id.get<std::string>()) ? trim(roles[id.get<std::string>()].get<std::string>()) : std::string(); };
    if (!role_of(a["id"]).empty()) p += "\n## Your job in this project\n" + role_of(a["id"]) + "\n";

    std::string team;
    for (auto& o : store.agents()) {
        if (o["id"] == a["id"] || !o.value("enabled", true)) continue;
        team += "- " + o["id"].get<std::string>() + ": " + o.value("name", "") + (o.value("role", "").empty() ? "" : ", " + o.value("role", ""));
        if (std::string r = role_of(o["id"]); !r.empty())
            team += "; in this project: " + truncate_utf8(r.substr(0, r.find('\n')), 120);
        if (o.value("reports_to", "") == a["id"].get<std::string>()) team += " (reports to you)";
        if (a.value("reports_to", "") == o["id"].get<std::string>()) team += " (your boss)";
        team += "\n";
    }
    if (!team.empty())
        p += "\n## Team\n" + team +
             "To ask a colleague for help put a block in your answer: <send to=\"agent_id\" title=\"Short title\">what you need</send>. "
             "Their answer comes back to you as a report; then answer the person. "
             "If you want a result, an answer or a report back, leave reply out. Add reply=\"no\" only for news that needs no answer at all "
             "(you will not hear back and cannot continue the work with their result). "
             "Only use ids from this list, never yourself. Text outside blocks is shown to the person who asked.\n";
    return p;
}

// ------------------------------------------------------------ reply parsing
std::string clean_title(const std::string& s) {
    std::string t = trim(s.substr(0, s.find('\n')));
    std::string out;
    for (char c : t) if (c != '<' && c != '>' && c != '"') out += c;
    return truncate_utf8(trim(out), 50);
}

static std::string attr(const std::string& tag, const std::string& name) {
    size_t p = tag.find(name + "=");
    if (p == std::string::npos) return "";
    p += name.size() + 1;
    if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) return "";
    char q = tag[p];
    size_t e = tag.find(q, p + 1);
    return e == std::string::npos ? "" : tag.substr(p + 1, e - p - 1);
}

ParsedReply parse_reply(const std::string& raw) {
    ParsedReply r;
    std::string text = raw;
    for (size_t pos = 0; (pos = text.find("<send", pos)) != std::string::npos;) {
        size_t gt = text.find('>', pos);
        size_t end = gt == std::string::npos ? std::string::npos : text.find("</send>", gt);
        if (gt == std::string::npos || end == std::string::npos) break;
        std::string tag = text.substr(pos, gt - pos);
        SendBlock b;
        b.to = attr(tag, "to");
        b.title = clean_title(attr(tag, "title"));
        b.reply = attr(tag, "reply") != "no";
        b.text = trim(text.substr(gt + 1, end - gt - 1));
        if (!b.to.empty() && !b.text.empty()) r.sends.push_back(std::move(b));
        text.erase(pos, end + 7 - pos);
    }
    size_t t1 = text.find("<title>"), t2 = text.find("</title>");
    if (t1 != std::string::npos && t2 != std::string::npos && t2 > t1) {
        r.title = clean_title(text.substr(t1 + 7, t2 - t1 - 7));
        text.erase(t1, t2 + 8 - t1);
    }
    r.text = trim(text);
    return r;
}

}  // namespace pc2
