#include "script_doc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace pc2::script {

namespace {

const std::set<std::string> kNodeTypes = {"start", "prompt", "set", "if", "delay", "end"};
const std::set<std::string> kReserved = {"and", "or", "not", "true", "false", "abs", "round", "min", "max"};

std::string str(const json& j, const char* key, size_t max, const std::string& fallback = "") {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return fallback;
    return truncate_utf8(j[key].get<std::string>(), max);
}

bool node_id_ok(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

bool var_name_ok(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

int coord(const json& n, const char* key) {
    if (!n.contains(key) || !n[key].is_number()) return 0;
    double v = n[key].get<double>();
    if (!std::isfinite(v)) return 0;
    return static_cast<int>(std::lround(std::clamp(v, -100000.0, 100000.0)));
}

json clean_props(const std::string& type, const json& p) {
    if (type == "prompt")
        return {{"agent_id", str(p, "agent_id", 64)}, {"project_id", str(p, "project_id", 64)},
                {"text", str(p, "text", 20000)}, {"target", str(p, "target", 32)}};
    if (type == "set") return {{"var", str(p, "var", 32)}, {"expr", str(p, "expr", 4000)}};
    if (type == "if") return {{"expr", str(p, "expr", 4000)}};
    if (type == "delay") {
        std::string mode = str(p, "mode", 8, "for");
        return {{"mode", mode == "until" ? "until" : "for"}, {"expr", str(p, "expr", 4000)}};
    }
    return json::object();
}

}  // namespace

const TypeEnv& builtin_types() {
    static const TypeEnv t = {
        {"now_time", Type::Time},          {"today", Type::Date},          {"claude_left_5h", Type::Number},
        {"claude_left_week", Type::Number}, {"codex_left", Type::Number}, {"tokens_today", Type::Number},
    };
    return t;
}

std::optional<json> clean_script(const json& in, json base, std::string& err) {
    if (!in.is_object()) { err = "A script must be a JSON object."; return std::nullopt; }
    if (!base.is_object()) base = json::object();
    base["version"] = 1;
    base["name"] = str(in, "name", 120, base.value("name", ""));
    base["description"] = str(in, "description", 2000, base.value("description", ""));
    base["project_id"] = str(in, "project_id", 64, base.value("project_id", ""));
    if (in.contains("draft") && in["draft"].is_boolean()) base["draft"] = in["draft"];
    if (!base.contains("draft")) base["draft"] = false;
    if (base["name"].get<std::string>().empty()) { err = "The script needs a name."; return std::nullopt; }

    if (in.contains("vars")) {
        if (!in["vars"].is_array() || in["vars"].size() > kMaxVars) { err = "Too many variables (at most 100)."; return std::nullopt; }
        json vars = json::array();
        for (const json& v : in["vars"]) {
            if (!v.is_object()) { err = "Every variable must be an object."; return std::nullopt; }
            vars.push_back({{"name", str(v, "name", 32)}, {"type", str(v, "type", 16, "Number")}, {"init", str(v, "init", 2000)}});
        }
        base["vars"] = vars;
    }
    if (!base.contains("vars")) base["vars"] = json::array();

    if (in.contains("nodes")) {
        if (!in["nodes"].is_array() || in["nodes"].size() > kMaxNodes) { err = "Too many blocks (at most 200)."; return std::nullopt; }
        json nodes = json::array();
        std::set<std::string> ids;
        for (const json& n : in["nodes"]) {
            if (!n.is_object()) { err = "Every block must be an object."; return std::nullopt; }
            std::string id = str(n, "id", 64), type = str(n, "type", 16);
            if (!node_id_ok(id)) { err = "Bad block id '" + id + "' (letters, digits, _ and -, at most 32)."; return std::nullopt; }
            if (!ids.insert(id).second) { err = "Two blocks have the id '" + id + "'."; return std::nullopt; }
            if (!kNodeTypes.count(type)) { err = "Unknown block type '" + type + "'."; return std::nullopt; }
            json node = {{"id", id}, {"type", type}, {"x", coord(n, "x")}, {"y", coord(n, "y")},
                         {"props", clean_props(type, n.value("props", json::object()))}};
            std::string label = str(n, "label", 80);
            if (!label.empty()) node["label"] = label;
            nodes.push_back(node);
        }
        base["nodes"] = nodes;
    }
    if (!base.contains("nodes")) base["nodes"] = json::array();

    if (in.contains("edges")) {
        if (!in["edges"].is_array() || in["edges"].size() > kMaxEdges) { err = "Too many arrows (at most 400)."; return std::nullopt; }
        json edges = json::array();
        for (const json& e : in["edges"]) {
            if (!e.is_object()) { err = "Every arrow must be an object."; return std::nullopt; }
            edges.push_back({{"from", str(e, "from", 32)}, {"port", str(e, "port", 8, "out")}, {"to", str(e, "to", 32)}});
        }
        base["edges"] = edges;
    }
    if (!base.contains("edges")) base["edges"] = json::array();
    return base;
}

std::vector<std::string> text_placeholders(const std::string& text) {
    std::vector<std::string> out;
    size_t k = 0;
    while ((k = text.find('{', k)) != std::string::npos) {
        size_t e = text.find('}', k + 1);
        if (e == std::string::npos) break;
        std::string name = text.substr(k + 1, e - k - 1);
        if (var_name_ok(name)) {
            out.push_back(name);
            k = e + 1;
        } else {
            k = k + 1;
        }
    }
    return out;
}

json check_script(const json& s, const ScriptLookup& lookup) {
    json errors = json::array();
    auto add = [&](std::string msg, json where = json::object()) {
        where["message"] = std::move(msg);
        errors.push_back(where);
    };
    int prompts = 0;

    // ------------------------------------------------------------ variables
    TypeEnv env = builtin_types();
    std::set<std::string> own_vars;
    for (const json& v : s.value("vars", json::array())) {
        std::string name = v.value("name", ""), type = v.value("type", ""), init = v.value("init", "");
        json where = {{"var", name}};
        if (!var_name_ok(name)) { add("Bad variable name '" + name + "' (letters, digits and _, not starting with a digit).", where); continue; }
        if (kReserved.count(name)) { add("'" + name + "' is a reserved word.", where); continue; }
        if (builtin_types().count(name)) { add("'" + name + "' is a built-in variable.", where); continue; }
        if (own_vars.count(name)) { add("Two variables are called '" + name + "'.", where); continue; }
        Type t = type_from_name(type);
        if (t == Type::Invalid) { add("Unknown type '" + type + "'.", where); continue; }
        Value val;
        if (!init.empty() && !parse_value(t, init, val)) {
            where["field"] = "init";
            add("Start value '" + init + "' is not a " + type + ".", where);
        }
        own_vars.insert(name);
        env[name] = t;
    }

    std::string script_project = s.value("project_id", "");
    if (!script_project.empty() && !lookup.project_exists(script_project)) add("Unknown project '" + script_project + "'.", {{"field", "project_id"}});

    // ---------------------------------------------------------------- blocks
    std::map<std::string, std::string> type_of;
    std::string start_id;
    int starts = 0;
    for (const json& n : s.value("nodes", json::array())) type_of[n["id"]] = n["type"];

    auto check_expr_at = [&](const json& where, const std::string& src, std::optional<Type> want, const char* what) -> std::optional<Type> {
        json w = where;
        w["field"] = "expr";
        if (src.empty()) { add(std::string(what) + " is empty.", w); return std::nullopt; }
        try {
            Type t = check_expr(parse_expr(src), env);
            if (want && t != *want) {
                add(std::string(what) + " must give a " + type_name(*want) + ", it gives a " + type_name(t) + ".", w);
                return std::nullopt;
            }
            return t;
        } catch (const ScriptError& e) {
            if (e.pos != std::string::npos) w["pos"] = e.pos;
            add(e.what(), w);
            return std::nullopt;
        }
    };
    auto writable = [&](const std::string& name, const json& where, const char* field) {
        json w = where;
        w["field"] = field;
        if (name.empty()) { add("Pick a variable.", w); return false; }
        if (builtin_types().count(name)) { add("'" + name + "' is built in and cannot be changed.", w); return false; }
        if (!own_vars.count(name)) { add("Unknown variable '" + name + "'.", w); return false; }
        return true;
    };

    for (const json& n : s.value("nodes", json::array())) {
        std::string id = n["id"], type = n["type"];
        const json& p = n["props"];
        json where = {{"node", id}};
        if (type == "start") {
            ++starts;
            start_id = id;
        } else if (type == "prompt") {
            ++prompts;
            std::string agent = p.value("agent_id", ""), project = p.value("project_id", ""), text = p.value("text", "");
            std::string target = p.value("target", "");
            if (agent.empty()) add("Pick a ghost.", {{"node", id}, {"field", "agent_id"}});
            else if (!lookup.agent_exists(agent)) add("Unknown ghost '" + agent + "'.", {{"node", id}, {"field", "agent_id"}});
            if (!project.empty() && !lookup.project_exists(project)) add("Unknown project '" + project + "'.", {{"node", id}, {"field", "project_id"}});
            if (text.empty()) add("The prompt is empty.", {{"node", id}, {"field", "text"}});
            for (const std::string& name : text_placeholders(text))
                if (!env.count(name)) add("Unknown variable {" + name + "} in the prompt.", {{"node", id}, {"field", "text"}});
            if (!target.empty()) writable(target, where, "target");
        } else if (type == "set") {
            std::string var = p.value("var", "");
            if (writable(var, where, "var")) check_expr_at(where, p.value("expr", ""), env[var], "The value");
            else if (!p.value("expr", "").empty()) check_expr_at(where, p.value("expr", ""), std::nullopt, "The value");
        } else if (type == "if") {
            check_expr_at(where, p.value("expr", ""), Type::Bool, "The condition");
        } else if (type == "delay") {
            bool until = p.value("mode", "for") == "until";
            check_expr_at(where, p.value("expr", ""), until ? Type::Time : Type::Number, until ? "The time" : "The minutes");
        }
    }
    if (starts == 0) add("The script needs a Start block.");
    if (starts > 1) add("A script has only one Start block.");

    // ---------------------------------------------------------------- arrows
    std::map<std::string, std::vector<std::string>> next;
    std::map<std::string, int> indeg;
    for (auto& [id, t] : type_of) indeg[id] = 0;
    std::set<std::string> seen_edges;
    int idx = 0;
    for (const json& e : s.value("edges", json::array())) {
        std::string from = e.value("from", ""), to = e.value("to", ""), port = e.value("port", "out");
        json where = {{"edge", idx++}};
        if (!type_of.count(from) || !type_of.count(to)) { add("An arrow points to a missing block.", where); continue; }
        const std::string& ft = type_of[from];
        bool port_ok = ft == "if" ? (port == "true" || port == "false") : (port == "out" && ft != "end");
        if (!port_ok) { add("Block '" + from + "' has no output '" + port + "'.", where); continue; }
        if (type_of[to] == "start") { add("Nothing can point to the Start block.", where); continue; }
        if (from == to) { add("A block cannot point to itself.", where); continue; }
        if (!seen_edges.insert(from + "\n" + port + "\n" + to).second) { add("The same arrow is there twice.", where); continue; }
        next[from].push_back(to);
        ++indeg[to];
    }

    // no loops: Kahn's algorithm, whatever is left sits on a cycle
    std::map<std::string, int> d = indeg;
    std::vector<std::string> queue;
    for (auto& [id, k] : d) if (k == 0) queue.push_back(id);
    size_t done = 0;
    while (done < queue.size()) {
        std::string id = queue[done++];
        for (const std::string& to : next[id])
            if (--d[to] == 0) queue.push_back(to);
    }
    if (done < d.size()) {
        for (auto& [id, k] : d)
            if (k > 0) add("Arrows go round in a loop here (loops are not allowed).", {{"node", id}});
    } else if (starts == 1) {
        std::set<std::string> reach = {start_id};
        std::vector<std::string> stack = {start_id};
        while (!stack.empty()) {
            std::string id = stack.back();
            stack.pop_back();
            for (const std::string& to : next[id])
                if (reach.insert(to).second) stack.push_back(to);
        }
        for (auto& [id, t] : type_of)
            if (!reach.count(id)) add("This block is not connected to Start, it would never run.", {{"node", id}});
    }

    return {{"errors", errors}, {"prompt_count", prompts}};
}

}  // namespace pc2::script
