#include "server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include "embedded.hpp"
#include "httplib.h"
#include "prompt.hpp"

namespace pc2 {

static httplib::Server* g_server = nullptr;

static const std::set<std::string> kForms = {"classic", "wizard", "pirate", "knight", "robot", "ninja", "chef", "detective", "royal", "cat", "scientist"};
static const std::set<std::string> kTools = {"Read", "Glob", "Grep", "Write", "Edit", "Bash", "PowerShell", "WebSearch", "WebFetch"};
static const std::set<std::string> kDirs = {"Agents", "Skills", "Rules", "Projects"};
static const std::set<std::string> kRoles = {"owner", "editor", "viewer"};

enum Rank { kPublic = -1, kViewer = 0, kEditor = 1, kOwner = 2 };

// ------------------------------------------------------------------ helpers
static void send(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(dumps(j), "application/json; charset=utf-8");
}
static void fail(httplib::Response& res, int status, const std::string& msg) { send(res, {{"error", msg}}, status); }

static bool parse_body(const httplib::Request& req, httplib::Response& res, json& out) {
    out = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (out.is_discarded() || !out.is_object()) { fail(res, 400, "Invalid JSON body"); return false; }
    return true;
}

static std::string str(const json& j, const char* k, size_t max, const std::string& def = "") {
    if (!j.contains(k) || !j[k].is_string()) return def;
    std::string s = j[k];
    return s.size() > max ? truncate_utf8(s, max) : s;
}
static std::string pct_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool is_color(const std::string& s) { return std::regex_match(s, std::regex("#[0-9a-fA-F]{6}")); }

static const char* mime_for(const std::string& path) {
    static const std::pair<const char*, const char*> t[] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"}, {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"}, {".ico", "image/x-icon"}};
    for (auto& [ext, m] : t)
        if (path.size() >= strlen(ext) && _stricmp(path.c_str() + path.size() - strlen(ext), ext) == 0) return m;
    return "application/octet-stream";
}

// A relative path inside `root`; empty when it would leave it.
static fs::path safe_path(const fs::path& root_in, const std::string& rel) {
    if (rel.empty()) return {};
    fs::path root = fs::weakly_canonical(root_in);
    fs::path p = fs::weakly_canonical(root / P(rel));
    auto r = p.lexically_relative(root);
    if (r.empty() || *r.begin() == L"..") return {};
    for (auto& part : r) if (part == L".Phantom") return {};  // project metadata is not a "file"
    return p;
}

static json list_addresses() {
    json out = json::array();
    ULONG len = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &len);
    std::vector<char> buf(len);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &len) != NO_ERROR) return out;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[64] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            std::string s = ip;
            if (s.rfind("169.254.", 0) == 0) continue;
            out.push_back({{"address", s}, {"name", Narrow(a->FriendlyName)}});
        }
    }
    return out;
}

// ---------------------------------------------------------------- context
struct Ctx {
    bool ok = false;
    std::string id, role, name;
    json profile;
};

// ---------------------------------------------------------------- presence
// Who has a window open: every signed-in request marks its user as seen. Open pages poll every few
// seconds (a hidden page still pings every 30 s), so a user not seen for kOnlineSeconds is "asleep".
// In memory only (static: survives the listener restart of the sharing switch).
constexpr std::time_t kOnlineSeconds = 75;
struct Seen { std::time_t at = 0; std::string iso; };
static std::mutex g_seen_mu;
static std::map<std::string, Seen> g_seen;
static void mark_seen(const std::string& id) {
    std::lock_guard<std::mutex> l(g_seen_mu);
    g_seen[id] = {std::time(nullptr), now_iso()};
}
static void forget_seen(const std::string& id) {
    std::lock_guard<std::mutex> l(g_seen_mu);
    auto it = g_seen.find(id);
    if (it != g_seen.end()) it->second.at = 0;  // offline now, keep the "last seen" time
}
// {online, last_seen}: last_seen (ISO) only while offline, so polling does not change the snapshot.
static std::pair<bool, std::string> presence_of(const std::string& id) {
    std::lock_guard<std::mutex> l(g_seen_mu);
    auto it = g_seen.find(id);
    if (it == g_seen.end()) return {false, ""};
    if (it->second.at && std::time(nullptr) - it->second.at <= kOnlineSeconds) return {true, ""};
    return {false, it->second.iso};
}

void stop_server() { if (g_server) g_server->stop(); }

int run_server(Store& store, Auth& auth, Runner& runner, const ServerOptions& opt, bool& restart) {
    httplib::Server svr;
    g_server = &svr;
    restart = false;
    Config& cfg = *opt.config;
    const std::string port = std::to_string(cfg.port);
    // Cookies belong to a host name, not to a port: two PC2 programs on one PC must not share a cookie name.
    const std::string cookie_name = "pc2_session_" + port + "=";
    const std::set<std::string> hosts = {"127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port};

    // ---- request context
    auto ctx_of = [&](const httplib::Request& req) {
        Ctx c;
        std::string cookie = req.get_header_value("Cookie"), tok;
        size_t p = cookie.find(cookie_name);
        if (p != std::string::npos) {
            p += cookie_name.size();
            size_t e = cookie.find(';', p);
            tok = cookie.substr(p, e == std::string::npos ? std::string::npos : e - p);
        }
        std::string id = tok.empty() ? "" : auth.session_user(tok);
        if (id.empty()) return c;
        auto u = store.user(id);
        if (!u) return c;
        c.ok = true;
        c.id = id;
        c.role = auth.role(id);
        c.name = u->value("name", id);
        c.profile = *u;
        mark_seen(id);
        return c;
    };
    auto public_user = [&](const json& u) {
        std::string id = u["id"].get<std::string>();
        auto [online, last_seen] = presence_of(id);
        json p{{"id", u["id"]}, {"name", u.value("name", "")}, {"form", u.value("form", "classic")},
               {"color", u.value("color", "#9ff0d4")}, {"role", auth.role(id)}, {"online", online}};
        if (!last_seen.empty()) p["last_seen"] = last_seen;
        return p;
    };
    auto set_cookie = [&](httplib::Response& res, const std::string& tok, int max_age) {
        res.set_header("Set-Cookie", cookie_name + tok + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(max_age));
    };

    // Conversation visibility: owner sees all; others see their own, shared ones and task conversations.
    auto visible = [&](const json& meta, const Ctx& c) {
        if (c.role == "owner") return true;
        return meta.value("user_id", "") == c.id || meta.value("shared", false) || meta.value("kind", "") == "task";
    };
    auto writable = [&](const json& meta, const Ctx& c) {
        std::string k = meta.value("kind", "");
        if (k != "user" && k != "task") return false;
        if (role_rank(c.role) < kEditor) return false;
        return meta.value("user_id", "") == c.id || meta.value("shared", false) || k == "task";
    };
    auto conv_visible = [&](const std::string& conv_id, const Ctx& c) {
        if (c.role == "owner") return true;
        auto cv = store.conv_load(conv_id);
        return cv && visible((*cv)["meta"], c);
    };

    // ---- security gate: this PC's own browser only, unless sharing is on
    // The tunnel program (cloudflared) connects from this PC, so tunnel traffic looks local: it is told apart by
    // its Host (the tunnel address) and by the headers Cloudflare adds.
    auto via_tunnel = [&](const httplib::Request& req) {
        if (!opt.tunnel) return false;
        std::string th = opt.tunnel->host();
        return !th.empty() && req.get_header_value("Host") == th;
    };
    auto forwarded = [&](const httplib::Request& req) {
        return req.has_header("Cf-Connecting-Ip") || req.has_header("X-Forwarded-For") || req.has_header("Cf-Ray");
    };
    auto is_this_pc = [&](const httplib::Request& req) {  // the person at this PC, not somebody on the network or the tunnel
        return (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1") && hosts.count(req.get_header_value("Host")) && !forwarded(req);
    };
    svr.set_pre_request_handler([&](const httplib::Request& req, httplib::Response& res) {
        if (!cfg.share && !hosts.count(req.get_header_value("Host")) && !via_tunnel(req)) {
            fail(res, 403, "Forbidden host");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method != "GET" && req.method != "HEAD") {
            std::string origin = req.get_header_value("Origin");
            std::string host = req.get_header_value("Host");
            bool origin_ok = origin.empty() || origin == "http://" + host || (via_tunnel(req) && origin == "https://" + host);
            if (!origin_ok || req.get_header_value("X-PC2") != "1") {
                fail(res, 403, "Forbidden");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "Internal error";
        try { if (ep) std::rethrow_exception(ep); } catch (const std::exception& e) { msg = e.what(); } catch (...) {}
        fail(res, 500, msg);
    });

    using Handler = std::function<void(const httplib::Request&, httplib::Response&, Ctx&)>;
    auto add = [&](const std::string& method, const std::string& pattern, int min_rank, Handler h) {
        auto wrap = [&, min_rank, h](const httplib::Request& req, httplib::Response& res) {
            Ctx c = ctx_of(req);
            if (min_rank >= kViewer) {
                if (!c.ok) return fail(res, 401, "Please sign in.");
                if (role_rank(c.role) < min_rank) return fail(res, 403, "Your role is not allowed to do this.");
            }
            h(req, res, c);
        };
        if (method == "GET") svr.Get(pattern, wrap);
        else if (method == "POST") svr.Post(pattern, wrap);
        else if (method == "PUT") svr.Put(pattern, wrap);
        else svr.Delete(pattern, wrap);
    };
    const std::string ID = "([A-Za-z0-9_-]+)";

    // ================================================================= auth
    add("GET", "/api/auth/status", kPublic, [&](auto&, auto& res, Ctx& c) {
        send(res, {{"setup_needed", !auth.has_users()}, {"me", c.ok ? public_user(c.profile) : json(nullptr)}});
    });

    auto start_session = [&](httplib::Response& res, const std::string& uid) {
        set_cookie(res, auth.new_session(uid), 30 * 86400);
        send(res, public_user(*store.user(uid)));
    };

    add("POST", "/api/auth/setup", kPublic, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        if (auth.has_users()) return fail(res, 409, "This company already has an owner.");
        std::string name = str(b, "name", 40), pw = str(b, "password", 200);
        if (name.empty()) return fail(res, 400, "Please enter your name.");
        if (!valid_password(pw)) return fail(res, 400, "The password needs at least 8 characters.");
        std::string form = str(b, "form", 16, "wizard"), color = str(b, "color", 7, "#b69cff");
        json u = store.save_user({{"name", name}, {"form", kForms.count(form) ? form : "classic"}, {"color", is_color(color) ? color : "#b69cff"}});
        auth.add_user(u["id"], "owner");
        auth.set_password(u["id"], pw);
        start_session(res, u["id"]);
    });

    // Extra owner account, only from the PC that runs the program (friends on the network must use invitations).
    add("POST", "/api/auth/register", kPublic, [&](auto& req, auto& res, Ctx&) {
        if (!is_this_pc(req)) return fail(res, 403, "New accounts can only be created on the PC that runs Phantom Co.");
        json b;
        if (!parse_body(req, res, b)) return;
        std::string name = str(b, "name", 40), pw = str(b, "password", 200);
        if (name.empty()) return fail(res, 400, "Please enter your name.");
        if (!valid_password(pw)) return fail(res, 400, "The password needs at least 8 characters.");
        for (auto& o : store.users()) if (lower(o.value("name", "")) == lower(name)) return fail(res, 409, "Someone already uses this name.");
        static const char* colors[] = {"#9ff0d4", "#ffb38a", "#7cc4ff", "#ff8fb8", "#ffe27a", "#b4f06a", "#f2f0ff", "#5fd6d6"};
        json u = store.save_user({{"name", name}, {"form", "classic"}, {"color", colors[store.users().size() % 8]}});
        auth.add_user(u["id"], "owner");
        auth.set_password(u["id"], pw);
        start_session(res, u["id"]);
    });

    add("POST", "/api/auth/login", kPublic, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string name = lower(str(b, "name", 40)), pw = str(b, "password", 200);
        std::string key = name + "|" + (req.has_header("Cf-Connecting-Ip") ? req.get_header_value("Cf-Connecting-Ip") : req.remote_addr);
        if (!auth.login_allowed(key)) return fail(res, 429, "Too many attempts. Wait a minute and try again.");
        std::string uid;
        for (auto& u : store.users()) if (lower(u.value("name", "")) == name) uid = u["id"];
        if (uid.empty() || !auth.exists(uid) || !auth.verify(uid, pw)) {
            auth.login_failed(key);
            return fail(res, 401, "Wrong name or password.");
        }
        auth.login_ok(key);
        start_session(res, uid);
    });

    add("POST", "/api/auth/logout", kPublic, [&](auto& req, auto& res, Ctx& c) {
        if (c.ok) forget_seen(c.id);
        std::string cookie = req.get_header_value("Cookie");
        size_t p = cookie.find(cookie_name);
        if (p != std::string::npos) auth.end_session(cookie.substr(p + cookie_name.size(), 48));
        set_cookie(res, "", 0);
        send(res, {{"ok", true}});
    });

    add("GET", "/api/auth/invite", kPublic, [&](auto& req, auto& res, Ctx&) {
        std::string uid = auth.invite_user(req.get_param_value("token"));
        auto u = uid.empty() ? std::nullopt : store.user(uid);
        if (!u) return fail(res, 404, "This invitation is not valid any more.");
        send(res, {{"name", u->value("name", "")}});
    });

    add("POST", "/api/auth/join", kPublic, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string token = str(b, "token", 64), pw = str(b, "password", 200);
        std::string uid = auth.invite_user(token);
        if (uid.empty() || !store.user(uid)) return fail(res, 404, "This invitation is not valid any more.");
        if (!valid_password(pw)) return fail(res, 400, "The password needs at least 8 characters.");
        auth.set_password(uid, pw);
        auth.use_invite(token);
        start_session(res, uid);
    });

    // ===================================================================== me
    add("PUT", "/api/me", kViewer, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        json u = c.profile;
        std::string name = str(b, "name", 40, u.value("name", ""));
        if (name.empty()) return fail(res, 400, "Please enter a name.");
        for (auto& o : store.users()) if (o["id"] != c.id && lower(o.value("name", "")) == lower(name)) return fail(res, 409, "Someone already uses this name.");
        u["name"] = name;
        std::string form = str(b, "form", 16, u.value("form", "classic")), color = str(b, "color", 7, u.value("color", "#9ff0d4"));
        if (!kForms.count(form)) return fail(res, 400, "Unknown ghost form.");
        if (!is_color(color)) return fail(res, 400, "Color must look like #rrggbb.");
        u["form"] = form;
        u["color"] = color;
        if (b.contains("password") && b["password"].is_string() && !b["password"].get<std::string>().empty()) {
            if (!auth.verify(c.id, str(b, "old_password", 200))) return fail(res, 403, "The current password is not right.");
            if (!valid_password(b["password"])) return fail(res, 400, "The new password needs at least 8 characters.");
            auth.set_password(c.id, b["password"]);
            set_cookie(res, auth.new_session(c.id), 30 * 86400);  // set_password ended the old sessions
        }
        send(res, public_user(store.save_user(u)));
    });

    // ================================================================= users
    add("GET", "/api/users", kOwner, [&](auto&, auto& res, Ctx&) {
        json list = json::array();
        for (auto& u : store.users()) {
            json p = public_user(u);
            p["has_password"] = auth.has_password(u["id"]);
            list.push_back(p);
        }
        send(res, {{"users", list}});
    });
    add("POST", "/api/users", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string name = str(b, "name", 40), role = str(b, "role", 10, "editor");
        if (name.empty()) return fail(res, 400, "Please enter a name.");
        if (!kRoles.count(role)) return fail(res, 400, "Unknown role.");
        for (auto& o : store.users()) if (lower(o.value("name", "")) == lower(name)) return fail(res, 409, "Someone already uses this name.");
        static const char* colors[] = {"#9ff0d4", "#ffb38a", "#7cc4ff", "#ff8fb8", "#ffe27a", "#b4f06a", "#f2f0ff", "#5fd6d6"};
        json u = store.save_user({{"name", name}, {"form", "classic"}, {"color", colors[store.users().size() % 8]}});
        auth.add_user(u["id"], role);
        send(res, {{"user", public_user(u)}, {"invite_token", auth.new_invite(u["id"])}}, 201);
    });
    add("PUT", "/api/users/" + ID, kOwner, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string id = req.matches[1];
        auto u = store.user(id);
        if (!u || !auth.exists(id)) return fail(res, 404, "User not found");
        std::string role = str(b, "role", 10, auth.role(id));
        if (!kRoles.count(role)) return fail(res, 400, "Unknown role.");
        if (auth.role(id) == "owner" && role != "owner" && auth.owner_count() <= 1) return fail(res, 409, "There must be at least one owner.");
        auth.set_role(id, role);
        std::string name = str(b, "name", 40, u->value("name", ""));
        if (!name.empty() && name != u->value("name", "")) {
            for (auto& o : store.users()) if (o["id"] != id && lower(o.value("name", "")) == lower(name)) return fail(res, 409, "Someone already uses this name.");
            (*u)["name"] = name;
            store.save_user(*u);
        }
        send(res, public_user(*store.user(id)));
    });
    add("POST", "/api/users/" + ID + "/invite", kOwner, [&](auto& req, auto& res, Ctx&) {
        std::string id = req.matches[1];
        if (!store.user(id) || !auth.exists(id)) return fail(res, 404, "User not found");
        send(res, {{"invite_token", auth.new_invite(id)}});
    });
    add("DELETE", "/api/users/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        std::string id = req.matches[1];
        if (!auth.exists(id)) return fail(res, 404, "User not found");
        if (auth.role(id) == "owner" && auth.owner_count() <= 1) return fail(res, 409, "There must be at least one owner.");
        auth.remove_user(id);
        store.remove_user(id);
        send(res, {{"ok", true}});
    });

    // ================================================================= state
    add("GET", "/api/state", kViewer, [&](auto&, auto& res, Ctx& c) {
        bool editor = role_rank(c.role) >= kEditor, owner = c.role == "owner";
        json providers = json::array();
        for (auto& p : detect_providers()) providers.push_back({{"id", p.id}, {"label", p.label}, {"available", p.available}});
        json act = runner.agent_activity(), agents = json::array();
        // tokens each agent used today (this PC's calendar day); run times are UTC
        std::map<std::string, long long> today_tokens;
        {
            std::time_t now = std::time(nullptr);
            std::tm lt = *std::localtime(&now);
            lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
            lt.tm_isdst = -1;
            long long day_start = (long long)std::mktime(&lt);
            for (auto& r : store.runs()) {  // newest first
                std::string created = r.value("created", "");
                std::tm ut = {};
                if (created.size() < 19 || sscanf(created.c_str(), "%d-%d-%dT%d:%d:%d", &ut.tm_year, &ut.tm_mon, &ut.tm_mday, &ut.tm_hour, &ut.tm_min, &ut.tm_sec) != 6) continue;
                ut.tm_year -= 1900; ut.tm_mon -= 1;
                if ((long long)_mkgmtime(&ut) < day_start) break;
                today_tokens[r.value("agent_id", "")] += r.value("tokens_in", 0LL) + r.value("tokens_out", 0LL);
            }
        }
        for (auto a : store.agents()) {
            a["tokens_today"] = today_tokens.count(a["id"].get<std::string>()) ? today_tokens[a["id"].get<std::string>()] : 0;
            a["activity"] = act.contains(a["id"].get<std::string>()) ? act[a["id"].get<std::string>()] : json(nullptr);
            agents.push_back(a);
        }
        json users = json::array();
        for (auto& u : store.users()) users.push_back(public_user(u));
        json skills = json::array();
        for (auto s : store.skills()) { if (!editor) s.erase("body"); skills.push_back(s); }
        json jobs = json::array();
        for (auto& j : runner.active_jobs()) if (conv_visible(j.value("conversation_id", ""), c)) jobs.push_back(j);
        json st = {{"company", store.company()}, {"me", public_user(c.profile)}, {"users", users}, {"providers", providers},
                   {"agents", agents}, {"skills", skills}, {"rules", store.rules()}, {"projects", store.projects()}, {"jobs", jobs},
                   {"forms", kForms}, {"tools", kTools}};
        {   // friends who lend their Claude/Codex (the agent editor offers "claude@<user>")
            json ws = json::array();
            for (auto& w : runner.hub().status()) {
                auto u = store.user(w.value("user_id", ""));
                if (!u) continue;
                json x = w;
                x["name"] = u->value("name", "");
                ws.push_back(x);
            }
            st["workers"] = ws;
        }
        if (owner) {
            st["settings"] = store.settings();
            st["vault_path"] = U8(store.root());
            st["share"] = cfg.share;
        }
        send(res, st);
    });

    add("PUT", "/api/company", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        json c = store.company();
        c["name"] = str(b, "name", 60, c.value("name", ""));
        c["goal"] = str(b, "goal", 2000, c.value("goal", ""));
        store.set_company(c);
        send(res, c);
    });

    add("PUT", "/api/settings", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b, patch = json::object();
        if (!parse_body(req, res, b)) return;
        auto num = [&](const char* k, int lo, int hi) {
            if (b.contains(k) && b[k].is_number_integer()) patch[k] = std::clamp(b[k].get<int>(), lo, hi);
        };
        num("max_hops", 1, 20);
        num("max_queue_per_user", 0, 100);
        num("max_concurrent", 1, 4);
        if (b.contains("default_model")) {
            std::string m = str(b, "default_model", 80);
            if (!std::regex_match(m, std::regex("[A-Za-z0-9._:/\\[\\]-]*"))) return fail(res, 400, "Model name has unsupported characters.");
            patch["default_model"] = m;
        }
        store.set_settings(patch);
        send(res, store.settings());
    });

    add("PUT", "/api/settings/vault", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string path = str(b, "path", 500);
        if (path.empty()) return fail(res, 400, "Give a folder path.");
        if (runner.any_active()) return fail(res, 409, "Wait until the running work has finished.");
        try { store.switch_root(P(path)); } catch (const std::exception& e) { return fail(res, 400, e.what()); }
        cfg.vault = P(path);
        save_config(cfg);
        send(res, {{"vault_path", U8(store.root())}});
    });
    add("POST", "/api/vault/open", kOwner, [&](auto&, auto& res, Ctx&) {
        ShellExecuteW(nullptr, L"open", store.root().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        send(res, {{"ok", true}});
    });

    add("GET", "/api/network", kOwner, [&](auto&, auto& res, Ctx&) {
        send(res, {{"share", cfg.share}, {"port", cfg.port}, {"addresses", list_addresses()}});
    });
    // Internet access through a Cloudflare quick tunnel (owner). Off at every start; the address changes every time.
    add("GET", "/api/tunnel", kOwner, [&](auto&, auto& res, Ctx&) {
        send(res, opt.tunnel ? opt.tunnel->status() : json{{"state", "error"}, {"installed", false}, {"url", ""}, {"message", "Not available."}});
    });
    add("POST", "/api/tunnel", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!opt.tunnel) return fail(res, 404, "Not available.");
        if (!parse_body(req, res, b)) return;
        std::string act = str(b, "action", 16);
        if (act == "install") opt.tunnel->Install();
        else if (act == "start") opt.tunnel->Start(cfg.port);
        else if (act == "stop") opt.tunnel->Stop();
        else return fail(res, 400, "Unknown action.");
        send(res, opt.tunnel->status());
    });
    add("PUT", "/api/network", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        bool share = b.value("share", false);
        if (share == cfg.share) return send(res, {{"share", cfg.share}, {"port", cfg.port}, {"addresses", list_addresses()}});
        cfg.share = share;
        save_config(cfg);
        restart = true;
        send(res, {{"share", cfg.share}, {"port", cfg.port}, {"addresses", list_addresses()}, {"restarting", true}});
        std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); stop_server(); }).detach();
    });

    // ================================================================ agents
    auto clean_agent = [&](const json& in, const json& base, json& out, std::string& err) {
        out = base;
        out["name"] = str(in, "name", 40, base.value("name", ""));
        out["role"] = str(in, "role", 60, base.value("role", ""));
        out["persona"] = str(in, "persona", 8000, base.value("persona", ""));
        out["provider"] = str(in, "provider", 64, base.value("provider", "claude"));
        out["model"] = str(in, "model", 80, base.value("model", ""));
        out["form"] = str(in, "form", 16, base.value("form", "classic"));
        out["color"] = str(in, "color", 7, base.value("color", "#b69cff"));
        out["reports_to"] = str(in, "reports_to", 64, base.value("reports_to", ""));
        out["enabled"] = in.contains("enabled") && in["enabled"].is_boolean() ? in["enabled"].get<bool>() : base.value("enabled", true);
        out["timeout_min"] = in.contains("timeout_min") && in["timeout_min"].is_number_integer() ? std::clamp(in["timeout_min"].get<int>(), 1, 240) : base.value("timeout_min", 30);
        out["skills"] = base.value("skills", json::array());
        if (in.contains("skills") && in["skills"].is_array()) {
            out["skills"] = json::array();
            for (auto& s : in["skills"]) if (s.is_string() && store.skill(s.get<std::string>())) out["skills"].push_back(s);
        }
        if (out["name"].get<std::string>().empty()) { err = "Every ghost needs a name."; return false; }
        {
            std::string pv = out["provider"], at_user;
            size_t at = pv.find('@');
            if (at != std::string::npos) { at_user = pv.substr(at + 1); pv = pv.substr(0, at); }
            if (!provider_known(pv) || (pv == "demo" && !at_user.empty())) { err = "Unknown provider."; return false; }
            if (at != std::string::npos && (!auth.exists(at_user) || role_rank(auth.role(at_user)) < kEditor)) { err = "That friend is not part of this company."; return false; }
        }
        if (!kForms.count(out["form"])) { err = "Unknown ghost form."; return false; }
        if (!is_color(out["color"])) { err = "Color must look like #rrggbb."; return false; }
        if (!std::regex_match(out["model"].get<std::string>(), std::regex("[A-Za-z0-9._:/\\[\\]-]*"))) { err = "Model name has unsupported characters."; return false; }
        std::string boss = out["reports_to"];
        if (!boss.empty()) {  // the boss must exist and the chain must not loop back
            std::string self = base.value("id", "");
            for (std::string cur = boss, hops = ""; !cur.empty(); hops += "x") {
                if (cur == self || hops.size() > 30) { err = "An agent cannot report to itself, directly or through others."; return false; }
                auto b = store.agent(cur);
                if (!b) { err = "The boss does not exist."; return false; }
                cur = b->value("reports_to", "");
            }
        }
        return true;
    };
    add("POST", "/api/agents", kEditor, [&](auto& req, auto& res, Ctx&) {
        json b, a;
        if (!parse_body(req, res, b)) return;
        std::string err;
        if (!clean_agent(b, json::object(), a, err)) return fail(res, 400, err);
        send(res, store.save_agent(a), 201);
    });
    add("PUT", "/api/agents/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        json b, a;
        if (!parse_body(req, res, b)) return;
        auto cur = store.agent(req.matches[1]);
        if (!cur) return fail(res, 404, "Agent not found");
        std::string err;
        if (!clean_agent(b, *cur, a, err)) return fail(res, 400, err);
        send(res, store.save_agent(a));
    });
    add("DELETE", "/api/agents/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        std::string id = req.matches[1];
        if (runner.agent_activity().contains(id)) return fail(res, 409, "This ghost is busy. Stop its work first.");
        if (!store.remove_agent(id)) return fail(res, 404, "Agent not found");
        for (auto o : store.agents()) if (o.value("reports_to", "") == id) { o["reports_to"] = ""; store.save_agent(o); }
        send(res, {{"ok", true}});
    });

    // ================================================================ skills
    auto clean_skill = [&](const json& in, const json& base, json& out, std::string& err) {
        out = base;
        out["name"] = str(in, "name", 60, base.value("name", ""));
        out["description"] = str(in, "description", 200, base.value("description", ""));
        out["body"] = str(in, "body", 8000, base.value("body", ""));
        out["enabled"] = in.contains("enabled") && in["enabled"].is_boolean() ? in["enabled"].get<bool>() : base.value("enabled", true);
        out["tools"] = json::array();
        out["dirs"] = json::array();
        for (auto& t : in.contains("tools") ? in["tools"] : base.value("tools", json::array()))
            if (t.is_string() && kTools.count(t.get<std::string>())) out["tools"].push_back(t);
        for (auto& d : in.contains("dirs") ? in["dirs"] : base.value("dirs", json::array()))
            if (d.is_string() && kDirs.count(d.get<std::string>())) out["dirs"].push_back(d);
        out["projects"] = json::array();  // empty = every project
        if (in.contains("projects") && in["projects"].is_array()) {
            for (auto& p : in["projects"])
                if (p.is_string() && store.project(p.get<std::string>())) out["projects"].push_back(p);
        } else out["projects"] = base.value("projects", json::array());
        // Powerful skills are owner-only: agents act with the rights of the person who asks.
        bool powerful = false;
        for (auto& t : out["tools"]) if (t == "Bash" || t == "PowerShell") powerful = true;
        for (auto& d : out["dirs"]) if (d != "Projects") powerful = true;
        out["min_role"] = powerful ? "owner" : str(in, "min_role", 10, base.value("min_role", "editor"));
        if (out["min_role"] != "owner" && out["min_role"] != "editor") out["min_role"] = "editor";
        if (out["name"].get<std::string>().empty()) { err = "The skill needs a name."; return false; }
        return true;
    };
    add("GET", "/api/skill_templates", kEditor, [&](auto&, auto& res, Ctx&) {
        json list = json::array();
        std::set<std::string> have;
        for (auto& s : store.skills()) have.insert(s["id"]);
        for (auto t : store.skill_templates()) { t["installed"] = have.count(t["id"].get<std::string>()) > 0; list.push_back(t); }
        send(res, {{"templates", list}});
    });
    add("POST", "/api/skills/from_template", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto s = store.install_skill_template(str(b, "id", 64));
        if (!s) return fail(res, 404, "Template not found");
        send(res, *s, 201);
    });
    add("POST", "/api/skills", kOwner, [&](auto& req, auto& res, Ctx&) {
        json b, s;
        if (!parse_body(req, res, b)) return;
        std::string err;
        if (!clean_skill(b, json::object(), s, err)) return fail(res, 400, err);
        send(res, store.save_skill(s), 201);
    });
    add("PUT", "/api/skills/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        json b, s;
        if (!parse_body(req, res, b)) return;
        auto cur = store.skill(req.matches[1]);
        if (!cur) return fail(res, 404, "Skill not found");
        std::string err;
        if (!clean_skill(b, *cur, s, err)) return fail(res, 400, err);
        send(res, store.save_skill(s));
    });
    add("DELETE", "/api/skills/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        if (!store.remove_skill(req.matches[1])) return fail(res, 404, "Skill not found");
        send(res, {{"ok", true}});
    });

    // ================================================================= rules
    auto save_rule_from = [&](const json& in, json base, httplib::Response& res, int status) {
        base["name"] = str(in, "name", 60, base.value("name", ""));
        base["body"] = str(in, "body", 2000, base.value("body", ""));
        base["enabled"] = in.contains("enabled") && in["enabled"].is_boolean() ? in["enabled"].get<bool>() : base.value("enabled", true);
        if (base["name"].get<std::string>().empty()) return fail(res, 400, "The rule needs a name.");
        send(res, store.save_rule(base), status);
    };
    add("POST", "/api/rules", kOwner, [&](auto& req, auto& res, Ctx&) { json b; if (parse_body(req, res, b)) save_rule_from(b, json::object(), res, 201); });
    add("PUT", "/api/rules/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto cur = store.rule(req.matches[1]);
        if (!cur) return fail(res, 404, "Rule not found");
        save_rule_from(b, *cur, res, 200);
    });
    add("DELETE", "/api/rules/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        if (!store.remove_rule(req.matches[1])) return fail(res, 404, "Rule not found");
        send(res, {{"ok", true}});
    });

    // =============================================================== projects
    add("POST", "/api/projects", kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string name = str(b, "name", 60);
        if (name.empty()) return fail(res, 400, "The project needs a name.");
        send(res, *store.project(store.save_project({{"name", name}, {"summary", str(b, "summary", 4000)}})["id"].get<std::string>()), 201);
    });
    add("PUT", "/api/projects/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto p = store.project(req.matches[1]);
        if (!p) return fail(res, 404, "Project not found");
        (*p)["name"] = str(b, "name", 60, p->value("name", ""));
        (*p)["summary"] = str(b, "summary", 4000, p->value("summary", ""));
        store.save_project(*p);
        send(res, *store.project(req.matches[1]));
    });
    add("DELETE", "/api/projects/" + ID, kOwner, [&](auto& req, auto& res, Ctx&) {
        std::string pid = req.matches[1];
        if (!store.project(pid)) return fail(res, 404, "Project not found");
        // the project's tasks (and their task chats) go with it; refuse while one of them is working
        std::vector<json> gone;
        for (auto& t : store.tasks())
            if (t.value("project_id", "") == pid) gone.push_back(t);
        for (auto& t : gone)
            if (runner.conversation_busy(t.value("conversation_id", ""))) return fail(res, 409, "Stop the work in this project's tasks first.");
        for (auto& t : gone) {
            store.conv_delete(t.value("conversation_id", ""));
            store.remove_task(t.value("id", ""));
        }
        if (!store.remove_project(pid)) return fail(res, 404, "Project not found");
        send(res, {{"ok", true}, {"tasks_removed", gone.size()}});
    });
    add("POST", "/api/projects/" + ID + "/notes", kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        if (!store.project(req.matches[1])) return fail(res, 404, "Project not found");
        std::string name = str(b, "name", 60);
        if (name.empty()) return fail(res, 400, "The note needs a title.");
        send(res, store.save_note(req.matches[1], {{"name", name}, {"body", str(b, "body", 4000)}, {"enabled", b.value("enabled", true)}}), 201);
    });
    add("PUT", "/api/projects/" + ID + "/notes/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto p = store.project(req.matches[1]);
        if (!p) return fail(res, 404, "Project not found");
        for (auto n : (*p)["notes"])
            if (n["id"] == req.matches[2].str()) {
                n["name"] = str(b, "name", 60, n.value("name", ""));
                n["body"] = str(b, "body", 4000, n.value("body", ""));
                n["enabled"] = b.contains("enabled") && b["enabled"].is_boolean() ? b["enabled"].get<bool>() : n.value("enabled", true);
                return send(res, store.save_note(req.matches[1], n));
            }
        fail(res, 404, "Note not found");
    });
    add("DELETE", "/api/projects/" + ID + "/notes/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        if (!store.remove_note(req.matches[1], req.matches[2])) return fail(res, 404, "Note not found");
        send(res, {{"ok", true}});
    });
    // What one ghost does in this project (added to its prompt there); empty text removes it.
    add("PUT", "/api/projects/" + ID + "/roles/" + ID, kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        if (!store.project(req.matches[1])) return fail(res, 404, "Project not found");
        if (!store.agent(req.matches[2])) return fail(res, 404, "Ghost not found");
        store.set_project_role(req.matches[1], req.matches[2], str(b, "text", 4000));
        send(res, *store.project(req.matches[1]));
    });

    // ================================================================= chats
    auto start_job = [&](const json& meta, const std::string& conv_id, Ctx& c, const std::string& text, bool name_chat, std::string& err) {
        Job j;
        j.kind = meta.value("kind", "") == "task" ? "task" : "chat";
        j.agent_id = meta.value("agent_id", "");
        j.user_id = c.id;
        j.user_role = c.role;
        j.task_id = meta.value("task_id", "");
        j.project_id = meta.value("project_id", "");
        j.stack = {{"user", c.id, conv_id, false}};
        j.prompt = meta.value("shared", false) && j.kind == "chat" ? "(" + c.name + ") " + text : text;
        j.name_chat = name_chat;
        return runner.enqueue(j, err);
    };
    auto chat_view = [&](const std::string& id, const Ctx& c) -> json {
        auto cv = store.conv_load(id);
        if (!cv || !visible((*cv)["meta"], c)) return json();
        json out = {{"meta", (*cv)["meta"]}, {"messages", (*cv)["messages"]}, {"busy", runner.conversation_busy(id)},
                    {"live", runner.conversation_live(id)}, {"can_write", writable((*cv)["meta"], c)}};
        return out;
    };

    add("GET", "/api/chats", kViewer, [&](auto&, auto& res, Ctx& c) {
        json list = json::array();
        for (auto& m : store.conv_list()) {
            if (!visible(m, c) || m.value("kind", "") == "task") continue;
            m["busy"] = runner.conversation_busy(m["id"]);
            list.push_back(m);
        }
        send(res, {{"chats", list}});
    });
    add("POST", "/api/chats", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string agent_id = str(b, "agent_id", 64), project_id = str(b, "project_id", 64);
        auto a = store.agent(agent_id);
        if (!a || !a->value("enabled", true)) return fail(res, 400, "Pick an agent to chat with.");
        if (!project_id.empty() && !store.project(project_id)) return fail(res, 400, "Unknown project.");
        std::string id = store.conv_create({{"kind", "user"}, {"title", "New chat with " + a->value("name", "")}, {"auto_title", true},
                                            {"agent_id", agent_id}, {"user_id", c.id}, {"project_id", project_id}, {"shared", false}});
        send(res, chat_view(id, c), 201);
    });
    add("GET", "/api/chats/" + ID, kViewer, [&](auto& req, auto& res, Ctx& c) {
        json v = chat_view(req.matches[1], c);
        if (v.is_null()) return fail(res, 404, "Chat not found");
        send(res, v);
    });
    add("POST", "/api/chats/" + ID + "/send", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string id = req.matches[1], text = str(b, "text", 20000);
        auto cv = store.conv_load(id);
        if (!cv || !visible((*cv)["meta"], c)) return fail(res, 404, "Chat not found");
        const json& meta = (*cv)["meta"];
        if (!writable(meta, c)) return fail(res, 403, "You cannot write in this conversation.");
        if (text.empty()) return fail(res, 400, "Write something first.");
        if (runner.conversation_busy(id)) return fail(res, 409, "The agent is still working here.");
        auto a = store.agent(meta.value("agent_id", ""));
        if (!a || !a->value("enabled", true)) return fail(res, 400, "The agent of this chat is not available.");
        bool first = true;
        for (auto& m : (*cv)["messages"]) if (m.value("from", "").rfind("agent:", 0) == 0) first = false;
        store.conv_append(id, {{"t", "msg"}, {"from", "user:" + c.id}, {"text", text}});
        std::string err, prompt = text;
        if (meta.value("kind", "") == "task" && !meta.value("task_id", "").empty() && (*cv)["messages"].size() <= 1) {
            if (auto t = store.task(meta.value("task_id", ""))) prompt = "# Task: " + t->value("title", "") + "\n\n" + text;
        }
        std::string rid = start_job(meta, id, c, prompt, first && meta.value("auto_title", false), err);
        if (rid.empty()) { store.conv_append(id, {{"t", "msg"}, {"from", "system"}, {"text", "⚠ " + err}}); return fail(res, 400, err); }
        send(res, chat_view(id, c));
    });
    add("POST", "/api/chats/" + ID + "/stop", kEditor, [&](auto& req, auto& res, Ctx& c) {
        auto cv = store.conv_load(req.matches[1]);
        if (!cv || !visible((*cv)["meta"], c)) return fail(res, 404, "Chat not found");
        if (!writable((*cv)["meta"], c) && c.role != "owner") return fail(res, 403, "You cannot stop work in this conversation.");
        runner.cancel_conversation(req.matches[1]);
        send(res, {{"ok", true}});
    });
    add("POST", "/api/chats/" + ID + "/reset", kEditor, [&](auto& req, auto& res, Ctx& c) {
        auto cv = store.conv_load(req.matches[1]);
        if (!cv || !visible((*cv)["meta"], c) || !writable((*cv)["meta"], c)) return fail(res, 404, "Chat not found");
        if (runner.conversation_busy(req.matches[1])) return fail(res, 409, "The agent is still working here.");
        store.conv_append(req.matches[1], {{"t", "reset"}, {"text", "New session: the agent forgot the earlier talk."}});
        send(res, chat_view(req.matches[1], c));
    });
    add("PUT", "/api/chats/" + ID, kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string id = req.matches[1];
        auto cv = store.conv_load(id);
        if (!cv || !visible((*cv)["meta"], c)) return fail(res, 404, "Chat not found");
        const json& meta = (*cv)["meta"];
        if (meta.value("user_id", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who started this chat can change it.");
        if (b.contains("title")) { std::string t = clean_title(str(b, "title", 80)); if (!t.empty()) store.conv_append(id, {{"t", "title"}, {"title", t}}); }
        if (b.contains("shared") && b["shared"].is_boolean() && meta.value("kind", "") == "user") store.conv_append(id, {{"t", "share"}, {"shared", b["shared"]}});
        if (b.contains("project_id") && b["project_id"].is_string()) {
            std::string pid = b["project_id"];
            if (!pid.empty() && !store.project(pid)) return fail(res, 400, "Unknown project.");
            store.conv_append(id, {{"t", "project"}, {"project_id", pid}});
        }
        send(res, chat_view(id, c));
    });
    add("DELETE", "/api/chats/" + ID, kEditor, [&](auto& req, auto& res, Ctx& c) {
        std::string id = req.matches[1];
        auto cv = store.conv_load(id);
        if (!cv || !visible((*cv)["meta"], c)) return fail(res, 404, "Chat not found");
        if ((*cv)["meta"].value("user_id", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who started this chat can delete it.");
        if (runner.conversation_busy(id)) return fail(res, 409, "Stop the work first.");
        store.conv_delete(id);
        send(res, {{"ok", true}});
    });

    // ================================================================= tasks
    // Reads a wake-up setting {wake: none|interval|daily, every_min, at} into t (wake, wake_every, wake_at). False + err when invalid.
    auto read_wake = [](const json& b, json& t, std::string& err) {
        std::string kind = str(b, "wake", 16);
        if (kind.empty() || kind == "none") {
            t["wake"] = ""; t["wake_every"] = 0; t["wake_at"] = "";
        } else if (kind == "interval") {
            long long every = b.contains("every_min") && b["every_min"].is_number() ? b["every_min"].get<long long>() : 0;
            if (every < 1 || every > 10080) { err = "Pick between 1 minute and 7 days (10080 minutes)."; return false; }
            t["wake"] = "interval"; t["wake_every"] = every; t["wake_at"] = "";
        } else if (kind == "daily") {
            std::string at = str(b, "at", 8);
            bool okat = at.size() == 5 && at[2] == ':' && isdigit((unsigned char)at[0]) && isdigit((unsigned char)at[1]) && isdigit((unsigned char)at[3]) && isdigit((unsigned char)at[4]) &&
                        std::stoi(at.substr(0, 2)) < 24 && std::stoi(at.substr(3, 2)) < 60;
            if (!okat) { err = "Give the time as HH:MM."; return false; }
            t["wake"] = "daily"; t["wake_at"] = at; t["wake_every"] = 0;
        } else { err = "Unknown wake-up kind."; return false; }
        return true;
    };
    auto task_view = [&](json t) {
        std::string conv = t.value("conversation_id", "");
        std::string status = runner.task_state(conv);
        if (status.empty()) {
            status = "idle";
            for (auto& r : store.runs())  // newest first
                if (r.value("conversation_id", "") == conv && (r.value("kind", "") == "task" || r.value("kind", "") == "report")) { status = r.value("status", "idle"); break; }
        }
        t["status"] = status;
        return t;
    };
    add("GET", "/api/tasks", kViewer, [&](auto&, auto& res, Ctx&) {
        json list = json::array();
        for (auto& t : store.tasks()) list.push_back(task_view(t));
        send(res, {{"tasks", list}});
    });
    add("POST", "/api/tasks", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string title = str(b, "title", 120), agent_id = str(b, "agent_id", 64), project_id = str(b, "project_id", 64);
        auto a = store.agent(agent_id);
        if (title.empty()) return fail(res, 400, "The task needs a title.");
        if (!a || !a->value("enabled", true)) return fail(res, 400, "Pick a ghost for this task.");
        if (!project_id.empty() && !store.project(project_id)) return fail(res, 400, "Unknown project.");
        std::string description = str(b, "description", 20000), werr;
        json t = {{"title", title}, {"description", description}, {"agent_id", agent_id}, {"project_id", project_id}, {"created_by", c.id}};
        if (b.contains("wake")) {  // optional wake-up, e.g. from a task template
            if (!read_wake(b, t, werr)) return fail(res, 400, werr);
            if (!t.value("wake", "").empty()) { t["wake_on"] = true; t["wake_last"] = std::to_string((long long)std::time(nullptr)); }
        }
        t = store.save_task(t);
        std::string conv = store.conv_create({{"kind", "task"}, {"title", title}, {"agent_id", agent_id}, {"user_id", c.id}, {"project_id", project_id},
                                              {"task_id", t["id"]}, {"shared", true}});
        t["conversation_id"] = conv;
        store.save_task(t);
        store.conv_append(conv, {{"t", "msg"}, {"from", "user:" + c.id}, {"text", description.empty() ? title : description}});
        if (b.value("run", true)) {
            std::string err;
            auto cv = store.conv_load(conv);
            if (start_job((*cv)["meta"], conv, c, "# Task: " + title + "\n\n" + description, false, err).empty()) {
                store.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", "⚠ " + err}});
                return fail(res, 400, err);
            }
        }
        send(res, task_view(t), 201);
    });
    add("GET", "/api/tasks/" + ID, kViewer, [&](auto& req, auto& res, Ctx& c) {
        auto t = store.task(req.matches[1]);
        if (!t) return fail(res, 404, "Task not found");
        json runs = json::array();
        for (auto& r : store.runs()) if (r.value("conversation_id", "") == t->value("conversation_id", "")) runs.push_back(r);
        send(res, {{"task", task_view(*t)}, {"runs", runs}});
    });
    add("POST", "/api/tasks/" + ID + "/run", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto t = store.task(req.matches[1]);
        if (!t) return fail(res, 404, "Task not found");
        std::string conv = t->value("conversation_id", "");
        auto cv = store.conv_load(conv);
        if (!cv) return fail(res, 404, "The task conversation is missing.");
        if (runner.conversation_busy(conv)) return fail(res, 409, "This task is already running.");
        std::string note = str(b, "note", 4000), err;
        if (!note.empty()) store.conv_append(conv, {{"t", "msg"}, {"from", "user:" + c.id}, {"text", note}});
        if (start_job((*cv)["meta"], conv, c, note.empty() ? "Please run this task again." : note, false, err).empty()) {
            store.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", "⚠ " + err}});
            return fail(res, 400, err);
        }
        send(res, task_view(*t), 201);
    });
    // Wake-up: the task runs again by itself, every N minutes or every day at a time (this PC's clock).
    add("PUT", "/api/tasks/" + ID + "/wake", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto t = store.task(req.matches[1]);
        if (!t) return fail(res, 404, "Task not found");
        if (t->value("created_by", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who created this task can change its wake-up.");
        std::string err;
        if (!read_wake(b, *t, err)) return fail(res, 400, err);
        if (t->value("wake", "").empty()) { (*t)["wake_on"] = false; (*t)["wake_last"] = ""; }
        else {
            (*t)["wake_on"] = b.value("on", true);
            (*t)["wake_last"] = std::to_string((long long)std::time(nullptr));  // counts from now
        }
        store.save_task(*t);
        send(res, task_view(*t));
    });
    add("DELETE", "/api/tasks/" + ID, kEditor, [&](auto& req, auto& res, Ctx& c) {
        auto t = store.task(req.matches[1]);
        if (!t) return fail(res, 404, "Task not found");
        if (t->value("created_by", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who created this task can delete it.");
        std::string conv = t->value("conversation_id", "");
        if (runner.conversation_busy(conv)) return fail(res, 409, "Stop the work first.");
        store.conv_delete(conv);
        store.remove_task(req.matches[1]);
        send(res, {{"ok", true}});
    });

    // ========================================================= task templates
    // A ready-made task (title, details, ghost, project, wake-up) that people start again and again.
    auto clean_template = [&](const json& in, json base, std::string& err) -> std::optional<json> {
        base["name"] = str(in, "name", 120, base.value("name", ""));
        base["description"] = str(in, "description", 20000, base.value("description", ""));
        base["agent_id"] = str(in, "agent_id", 64, base.value("agent_id", ""));
        base["project_id"] = str(in, "project_id", 64, base.value("project_id", ""));
        if (base["name"].get<std::string>().empty()) { err = "The template needs a name."; return std::nullopt; }
        if (!base["agent_id"].get<std::string>().empty() && !store.agent(base["agent_id"])) { err = "Unknown ghost."; return std::nullopt; }
        if (!base["project_id"].get<std::string>().empty() && !store.project(base["project_id"])) { err = "Unknown project."; return std::nullopt; }
        if (in.contains("wake") && !read_wake(in, base, err)) return std::nullopt;
        return base;
    };
    add("GET", "/api/task_templates", kViewer, [&](auto&, auto& res, Ctx&) { send(res, {{"templates", store.task_templates()}}); });
    add("POST", "/api/task_templates", kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        std::string err;
        auto t = clean_template(b, {{"created_by", c.id}}, err);
        if (!t) return fail(res, 400, err);
        send(res, store.save_task_template(*t), 201);
    });
    add("PUT", "/api/task_templates/" + ID, kEditor, [&](auto& req, auto& res, Ctx& c) {
        json b;
        if (!parse_body(req, res, b)) return;
        auto cur = store.task_template(req.matches[1]);
        if (!cur) return fail(res, 404, "Template not found");
        if (cur->value("created_by", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who made this template can change it.");
        std::string err;
        auto t = clean_template(b, *cur, err);
        if (!t) return fail(res, 400, err);
        send(res, store.save_task_template(*t));
    });
    add("DELETE", "/api/task_templates/" + ID, kEditor, [&](auto& req, auto& res, Ctx& c) {
        auto cur = store.task_template(req.matches[1]);
        if (!cur) return fail(res, 404, "Template not found");
        if (cur->value("created_by", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who made this template can delete it.");
        store.remove_task_template(req.matches[1]);
        send(res, {{"ok", true}});
    });

    // ================================================================== runs
    add("GET", "/api/runs", kViewer, [&](auto&, auto& res, Ctx& c) {
        json list = json::array();
        std::map<std::string, bool> seen;
        for (auto& r : store.runs()) {
            std::string conv = r.value("conversation_id", "");
            if (!seen.count(conv)) seen[conv] = conv_visible(conv, c);
            if (!seen[conv]) continue;
            list.push_back(r);
            if (list.size() >= 200) break;
        }
        send(res, {{"runs", list}});
    });
    add("GET", "/api/runs/" + ID, kViewer, [&](auto& req, auto& res, Ctx& c) {
        auto r = store.run_get(req.matches[1]);
        if (!r || !conv_visible(r->value("conversation_id", ""), c)) return fail(res, 404, "Run not found");
        send(res, *r);
    });
    add("GET", "/api/runs/" + ID + "/events", kViewer, [&](auto& req, auto& res, Ctx& c) {
        auto r = store.run_get(req.matches[1]);
        if (!r || !conv_visible(r->value("conversation_id", ""), c)) return fail(res, 404, "Run not found");
        size_t after = req.has_param("after") ? (size_t)std::max(0, atoi(req.get_param_value("after").c_str())) : 0;
        send(res, runner.events(req.matches[1], after));
    });
    add("POST", "/api/runs/" + ID + "/cancel", kEditor, [&](auto& req, auto& res, Ctx& c) {
        auto r = store.run_get(req.matches[1]);
        if (!r || !conv_visible(r->value("conversation_id", ""), c)) return fail(res, 404, "Run not found");
        if (r->value("user_id", "") != c.id && c.role != "owner") return fail(res, 403, "Only the person who started this run can stop it.");
        if (!runner.cancel_run(req.matches[1])) return fail(res, 404, "This run is not active.");
        send(res, {{"ok", true}});
    });

    // ================================================================= files
    auto scope_root = [&](const std::string& scope, fs::path& root) {
        if (scope.empty() || scope == "shared") { root = store.files_root(); return true; }
        if (scope.rfind("project:", 0) == 0 && store.project(scope.substr(8))) { root = store.project_dir(scope.substr(8)); return true; }
        return false;
    };
    add("GET", "/api/files", kViewer, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        json list = json::array();
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || list.size() >= 3000) break;
            if (it->path().filename() == L".Phantom") { it.disable_recursion_pending(); continue; }
            bool dir = it->is_directory(ec);
            std::string rel = Narrow(it->path().lexically_relative(root).generic_wstring());
            list.push_back({{"path", rel}, {"dir", dir}, {"size", dir ? 0 : (int64_t)it->file_size(ec)}});
        }
        send(res, {{"files", list}});
    });
    add("GET", "/api/files/read", kViewer, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, req.get_param_value("path"));
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) return fail(res, 404, "File not found");
        std::string data;
        read_file(p, data);
        bool binary = data.find('\0') != std::string::npos;
        send(res, {{"binary", binary}, {"size", data.size()}, {"truncated", data.size() > 200000}, {"text", binary ? "" : data.substr(0, 200000)}});
    });
    add("GET", "/api/files/raw", kViewer, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, req.get_param_value("path"));
        std::error_code ec;
        const char* m = mime_for(U8(p.filename()));
        if (p.empty() || !fs::is_regular_file(p, ec) || std::string(m).rfind("image/", 0) != 0) return fail(res, 404, "Not an image");
        std::string data;
        read_file(p, data);
        res.set_header("Content-Security-Policy", "sandbox");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_content(data, m);
    });

    // ======================================================= friends' PCs lend their AI
    // The friend's program signs in with a worker token (not a cookie), asks for jobs, streams events back and
    // mirrors the run's folder. Every call after "poll" is tied to a run that this worker currently has.
    auto worker_of = [&](const httplib::Request& req) -> std::string {
        std::string h = req.get_header_value("Authorization");
        if (h.rfind("Bearer ", 0) != 0) return "";
        std::string id = auth.worker_user(h.substr(7));
        return !id.empty() && role_rank(auth.role(id)) >= kEditor ? id : "";
    };
    add("POST", "/api/worker/token", kEditor, [&](auto&, auto& res, Ctx& c) {
        send(res, {{"token", auth.new_worker_token(c.id)}});
    });
    add("DELETE", "/api/worker/token", kEditor, [&](auto&, auto& res, Ctx& c) {
        auth.end_worker(c.id);
        send(res, {{"ok", true}});
    });
    add("POST", "/api/worker/poll", kPublic, [&](auto& req, auto& res, Ctx&) {
        std::string w = worker_of(req);
        if (w.empty()) return fail(res, 401, "Unknown worker token.");
        json b;
        if (!parse_body(req, res, b)) return;
        std::set<std::string> provs;
        if (b.contains("providers") && b["providers"].is_array())
            for (auto& p : b["providers"]) if (p.is_string() && (p == "claude" || p == "codex")) provs.insert(p.get<std::string>());
        auto job = runner.hub().poll(w, provs, 20);
        if (!job) return send(res, {{"job", nullptr}});
        json j = job->payload;
        j["run_id"] = job->run_id;
        send(res, {{"job", j}});
    });
    add("POST", "/api/worker/event", kPublic, [&](auto& req, auto& res, Ctx&) {
        std::string w = worker_of(req);
        json b;
        if (w.empty()) return fail(res, 401, "Unknown worker token.");
        if (!parse_body(req, res, b)) return;
        auto job = runner.hub().find(str(b, "run_id", 64), w);
        if (!job) return fail(res, 404, "This run is not waiting for you any more.");
        runner.hub().touch(job);
        if (b.contains("events") && b["events"].is_array() && b["events"].size() <= 200)
            for (auto& ev : b["events"]) if (job->on_event) job->on_event(ev);
        send(res, {{"cancel", job->cancel.load()}});
    });
    add("POST", "/api/worker/finish", kPublic, [&](auto& req, auto& res, Ctx&) {
        std::string w = worker_of(req);
        json b;
        if (w.empty()) return fail(res, 401, "Unknown worker token.");
        if (!parse_body(req, res, b)) return;
        auto job = runner.hub().find(str(b, "run_id", 64), w);
        if (!job) return fail(res, 404, "This run is not waiting for you any more.");
        runner.hub().finish(job, b.contains("result") && b["result"].is_object() ? b["result"] : json{{"status", "failed"}, {"error", "The worker sent no result."}});
        send(res, {{"ok", true}});
    });

    // Files of the run's folder. hash = SHA-256; cached by (size, time) because big folders are listed for every run.
    struct HashEntry { int64_t size = 0; long long time = 0; std::string hash; };
    auto hash_cache = std::make_shared<std::map<std::string, HashEntry>>();
    auto hash_mu = std::make_shared<std::mutex>();
    constexpr int64_t kMirrorFileMax = 50ll * 1024 * 1024;
    auto run_root = [&](const httplib::Request& req, httplib::Response& res, fs::path& root) -> std::shared_ptr<RemoteJob> {
        std::string w = worker_of(req);
        if (w.empty()) { fail(res, 401, "Unknown worker token."); return nullptr; }
        auto job = runner.hub().find(req.get_param_value("run_id"), w);
        if (!job) { fail(res, 404, "This run is not waiting for you any more."); return nullptr; }
        if (!scope_root(job->scope, root)) { fail(res, 404, "The folder of this run is gone."); return nullptr; }
        runner.hub().touch(job);
        return job;
    };
    add("GET", "/api/worker/files", kPublic, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!run_root(req, res, root)) return;
        json list = json::array();
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || list.size() >= 5000) break;
            if (it->path().filename() == L".Phantom") { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            int64_t size = (int64_t)it->file_size(ec);
            if (size > kMirrorFileMax) continue;
            long long t = (long long)it->last_write_time(ec).time_since_epoch().count();
            std::string rel = Narrow(it->path().lexically_relative(root).generic_wstring()), hash;
            {
                std::lock_guard<std::mutex> lk(*hash_mu);
                auto c = hash_cache->find(U8(it->path()));
                if (c != hash_cache->end() && c->second.size == size && c->second.time == t) hash = c->second.hash;
            }
            if (hash.empty()) {
                std::string data;
                if (!read_file(it->path(), data)) continue;
                hash = sha256_hex_of(data);
                std::lock_guard<std::mutex> lk(*hash_mu);
                if (hash_cache->size() > 20000) hash_cache->clear();
                (*hash_cache)[U8(it->path())] = {size, t, hash};
            }
            list.push_back({{"path", rel}, {"size", size}, {"hash", hash}});
        }
        send(res, {{"files", list}});
    });
    add("GET", "/api/worker/file", kPublic, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!run_root(req, res, root)) return;
        fs::path p = safe_path(root, req.get_param_value("path"));
        std::error_code ec;
        std::string data;
        if (p.empty() || !fs::is_regular_file(p, ec) || !read_file(p, data)) return fail(res, 404, "File not found");
        res.set_content(data, "application/octet-stream");
    });
    add("PUT", "/api/worker/file", kPublic, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!run_root(req, res, root)) return;
        fs::path p = safe_path(root, req.get_param_value("path"));
        if (p.empty() || (int64_t)req.body.size() > kMirrorFileMax) return fail(res, 400, "That file is not allowed.");
        std::error_code ec;
        if (fs::is_directory(p, ec)) return fail(res, 409, "A folder with this name exists.");
        fs::create_directories(p.parent_path(), ec);
        if (!write_file_atomic(p, req.body)) return fail(res, 500, "The file could not be saved.");
        send(res, {{"ok", true}});
    });

    // File exchange: editors put files into the shared folder / a project folder and everybody can take them.
    constexpr size_t kMaxUpload = 200u * 1024 * 1024;
    add("GET", "/api/files/download", kViewer, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, req.get_param_value("path"));
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) return fail(res, 404, "File not found");
        std::string data;
        if (!read_file(p, data)) return fail(res, 500, "The file could not be read.");
        std::string name = U8(p.filename());
        for (char& ch : name) if (ch == '"' || ch == '\\' || (unsigned char)ch < 32) ch = '_';
        res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"; filename*=UTF-8''" + pct_encode(name));
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Content-Security-Policy", "sandbox");
        res.set_content(data, "application/octet-stream");
    });
    add("PUT", "/api/files/upload", kEditor, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, req.get_param_value("path"));
        if (p.empty()) return fail(res, 400, "That file name is not allowed.");
        if (req.body.size() > kMaxUpload) return fail(res, 413, "The file is too large (limit 200 MB).");
        std::error_code ec;
        if (fs::is_directory(p, ec)) return fail(res, 409, "A folder with this name exists.");
        if (fs::exists(p, ec) && req.get_param_value("overwrite") != "1") return fail(res, 409, "A file with this name already exists.");
        fs::create_directories(p.parent_path(), ec);
        if (!write_file_atomic(p, req.body)) return fail(res, 500, "The file could not be saved.");
        send(res, {{"ok", true}, {"size", req.body.size()}}, 201);
    });
    add("POST", "/api/files/mkdir", kEditor, [&](auto& req, auto& res, Ctx&) {
        json b;
        if (!parse_body(req, res, b)) return;
        fs::path root;
        if (!scope_root(str(b, "scope", 80), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, str(b, "path", 300));
        if (p.empty()) return fail(res, 400, "That folder name is not allowed.");
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec) return fail(res, 500, "The folder could not be created.");
        send(res, {{"ok", true}}, 201);
    });
    add("DELETE", "/api/files", kOwner, [&](auto& req, auto& res, Ctx&) {
        fs::path root;
        if (!scope_root(req.get_param_value("scope"), root)) return fail(res, 404, "Unknown folder");
        fs::path p = safe_path(root, req.get_param_value("path"));
        std::error_code ec;
        if (p.empty() || !fs::exists(p, ec)) return fail(res, 404, "Not found");
        fs::remove_all(p, ec);
        if (ec) return fail(res, 500, "It could not be deleted.");
        send(res, {{"ok", true}});
    });

    // ============================================================== static UI
    add("GET", "/(.*)", kPublic, [&](auto& req, auto& res, Ctx&) {
        std::string path = req.matches[1];
        if (path.empty()) path = "index.html";
        if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) return fail(res, 404, "Not found");
        res.set_header("Cache-Control", "no-store");
        if (!opt.ui_dir.empty()) {
            std::string data;
            if (read_file(opt.ui_dir / P(path), data)) return res.set_content(data, mime_for(path));
        } else {
            for (size_t i = 0; i < kEmbeddedCount; ++i)
                if (std::string(kEmbedded[i].group) == "ui" && path == kEmbedded[i].path)
                    return res.set_content(std::string(reinterpret_cast<const char*>(kEmbedded[i].data), kEmbedded[i].size), mime_for(path));
        }
        fail(res, 404, "Not found");
    });

    if (!svr.bind_to_port(cfg.share ? "0.0.0.0" : "127.0.0.1", cfg.port)) {
        std::cerr << "Cannot listen on port " << cfg.port << " (is another copy already running? try --port)\n";
        g_server = nullptr;
        if (opt.on_listening) opt.on_listening(false);
        return 2;
    }
    if (opt.on_listening) opt.on_listening(true);

    // ---- wake-ups: every 15 s look for tasks whose time has come and run them with the rights of the person who made the task
    std::atomic<bool> scheduler_stop{false};
    std::thread scheduler([&] {
        auto wake_due = [](const json& t, long long now) {
            long long last = 0;
            try { last = std::stoll(t.value("wake_last", "0")); } catch (...) {}
            std::string kind = t.value("wake", "");
            if (kind == "interval") return t.value("wake_every", 0) > 0 && now >= last + (long long)t.value("wake_every", 0) * 60;
            if (kind != "daily" || t.value("wake_at", "").size() != 5) return false;
            std::time_t tt = (std::time_t)now;
            std::tm lt = *std::localtime(&tt);
            lt.tm_hour = std::stoi(t["wake_at"].get<std::string>().substr(0, 2));
            lt.tm_min = std::stoi(t["wake_at"].get<std::string>().substr(3, 2));
            lt.tm_sec = 0;
            lt.tm_isdst = -1;
            long long slot = (long long)std::mktime(&lt);
            return now >= slot && last < slot && now - slot < 3600;  // a slot missed by more than an hour (program was closed) is skipped
        };
        for (int tick = 0; !scheduler_stop; ++tick) {
            for (int i = 0; i < 15 && !scheduler_stop; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
            if (scheduler_stop) break;
            try {
                long long now = (long long)std::time(nullptr);
                for (auto& t : store.tasks()) {
                    if (!t.value("wake_on", false) || t.value("wake", "").empty() || !wake_due(t, now)) continue;
                    std::string conv = t.value("conversation_id", "");
                    auto cv = store.conv_load(conv);
                    auto u = store.user(t.value("created_by", ""));
                    auto a = store.agent(t.value("agent_id", ""));
                    if (!cv || runner.conversation_busy(conv)) continue;  // busy: try again at the next look
                    Ctx c;
                    if (u && !u->value("disabled", false)) { c.ok = true; c.id = u->value("id", ""); c.role = auth.role(c.id); c.name = u->value("name", c.id); }
                    std::string err;
                    if (!c.ok || c.role == "viewer") err = "the person who made this task cannot run it any more.";
                    else if (!a || !a->value("enabled", true)) err = "its ghost is gone or switched off.";
                    else {
                        store.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", "⏰ Wake-up: running this task again."}});
                        start_job((*cv)["meta"], conv, c, "Scheduled wake-up: please run this task again.", false, err);
                        if (!err.empty()) err = "it could not start (" + err + ")";
                    }
                    if (!err.empty()) store.conv_append(conv, {{"t", "msg"}, {"from", "system"}, {"text", "⚠ Wake-up skipped: " + err}});
                    t["wake_last"] = std::to_string(now);
                    store.save_task(t);
                }
            } catch (...) { /* a broken task file must not stop the clock */ }
        }
    });

    svr.listen_after_bind();
    scheduler_stop = true;
    scheduler.join();
    g_server = nullptr;
    return 0;
}

}  // namespace pc2
