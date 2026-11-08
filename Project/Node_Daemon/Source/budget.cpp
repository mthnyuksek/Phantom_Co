#include "budget.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>

namespace pc2 {

long long iso_to_unix(const std::string& iso) {
    std::tm ut = {};
    if (iso.size() < 19 || sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &ut.tm_year, &ut.tm_mon, &ut.tm_mday, &ut.tm_hour, &ut.tm_min, &ut.tm_sec) != 6) return 0;
    ut.tm_year -= 1900;
    ut.tm_mon -= 1;
    return (long long)_mkgmtime(&ut);
}

static std::tm local_tm(long long t) {
    std::time_t tt = (std::time_t)t;
    std::tm lt = {};
    localtime_s(&lt, &tt);
    return lt;
}

struct Periods { long long day = 0, month = 0; std::string day_key, month_key; };
static Periods periods_now() {
    Periods p;
    std::tm lt = local_tm((long long)std::time(nullptr));
    char buf[16];
    strftime(buf, sizeof buf, "%Y-%m-%d", &lt);
    p.day_key = buf;
    p.month_key = p.day_key.substr(0, 7);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    p.day = (long long)std::mktime(&lt);
    lt.tm_mday = 1;
    lt.tm_isdst = -1;
    p.month = (long long)std::mktime(&lt);
    return p;
}

static double run_cost(const json& r) {
    const std::string c = r.value("cost", "");
    return c.empty() ? 0.0 : std::max(0.0, strtod(c.c_str(), nullptr));
}
static long long run_tokens(const json& r) { return r.value("tokens_in", 0LL) + r.value("tokens_out", 0LL); }

struct Spend { long long day_tokens = 0, month_tokens = 0; double day_usd = 0, month_usd = 0; };
static void add_spend(Spend& s, const json& r, bool today) {
    const long long t = run_tokens(r);
    const double c = run_cost(r);
    s.month_tokens += t;
    s.month_usd += c;
    if (today) { s.day_tokens += t; s.day_usd += c; }
}
static json spend_json(const Spend& s) {
    return {{"day_tokens", s.day_tokens}, {"month_tokens", s.month_tokens}, {"day_usd", s.day_usd}, {"month_usd", s.month_usd}};
}

static json clean_limit(const json& in) {
    json out = json::object();
    if (!in.is_object()) return out;
    for (const char* k : {"day_tokens", "month_tokens"})
        if (in.contains(k) && in[k].is_number()) {
            const double v = in[k].get<double>();
            if (v >= 1 && v <= 1e15) out[k] = (long long)v;
        }
    for (const char* k : {"day_usd", "month_usd"})
        if (in.contains(k) && in[k].is_number()) {
            const double v = in[k].get<double>();
            if (v > 0 && v <= 1e7) out[k] = v;
        }
    return out;
}

json clean_budgets(const json& in, Store& store) {
    json out = {{"company", json::object()}, {"users", json::object()}, {"agents", json::object()}};
    if (!in.is_object()) return out;
    if (in.contains("company")) out["company"] = clean_limit(in["company"]);
    if (in.contains("users") && in["users"].is_object())
        for (auto& [id, l] : in["users"].items()) {
            json c = clean_limit(l);
            if (!c.empty() && store.user(id)) out["users"][id] = c;
        }
    if (in.contains("agents") && in["agents"].is_object())
        for (auto& [id, l] : in["agents"].items()) {
            json c = clean_limit(l);
            if (!c.empty() && store.agent(id)) out["agents"][id] = c;
        }
    return out;
}

static std::string token_text(long long n) { return std::to_string(n) + " token"; }
static std::string usd_text(double v) {
    char buf[40];
    snprintf(buf, sizeof buf, "$%.2f", v);
    return buf;
}

// Over one of the four limits of a scope? `who` / `who_tr`: "the company's" or "Quill's".
static bool over_limit(const json& limit, const Spend& s, const std::string& scope, const std::string& who, const std::string& who_tr, const Periods& p, BudgetVerdict& v) {
    struct Check { const char* key; bool day; bool usd; };
    static const Check checks[] = {{"day_tokens", true, false}, {"day_usd", true, true}, {"month_tokens", false, false}, {"month_usd", false, true}};
    for (const Check& c : checks) {
        if (!limit.contains(c.key)) continue;
        std::string amount;
        if (c.usd) {
            const double lim = limit[c.key].get<double>(), used = c.day ? s.day_usd : s.month_usd;
            if (used < lim) continue;
            amount = usd_text(used) + " / " + usd_text(lim);
        } else {
            const long long lim = limit[c.key].get<long long>(), used = c.day ? s.day_tokens : s.month_tokens;
            if (used < lim) continue;
            amount = token_text(used) + " / " + token_text(lim);
        }
        v.blocked = true;
        v.text = "Budget reached: " + who + (c.day ? " daily" : " monthly") + " limit (" + amount + "). The owner can change it in Overview.";
        v.text_tr = "Bütçe doldu: " + who_tr + (c.day ? " günlük" : " aylık") + " limiti (" + amount + "). Sahibi bunu Genel Bakış’ta değiştirebilir.";
        v.key = "budget:" + scope + ":" + c.key + ":" + (c.day ? p.day_key : p.month_key);
        return true;
    }
    return false;
}

BudgetVerdict budget_check(Store& store, const std::string& agent_id, const std::string& user_id) {
    BudgetVerdict v;
    json b = store.budgets();
    const json company = b.value("company", json::object());
    const json users = b.value("users", json::object()), agents = b.value("agents", json::object());
    const json user_limit = users.contains(user_id) ? users[user_id] : json::object();
    const json agent_limit = agents.contains(agent_id) ? agents[agent_id] : json::object();
    if (company.empty() && user_limit.empty() && agent_limit.empty()) return v;

    Periods p = periods_now();
    Spend c, u, a;
    for (auto& r : store.runs()) {  // newest first
        const long long t = iso_to_unix(r.value("created", ""));
        if (t < p.month) break;
        const bool today = t >= p.day;
        add_spend(c, r, today);
        if (r.value("user_id", "") == user_id) add_spend(u, r, today);
        if (r.value("agent_id", "") == agent_id) add_spend(a, r, today);
    }
    if (over_limit(company, c, "company", "the company's", "şirketin", p, v)) return v;
    if (!user_limit.empty()) {
        auto person = store.user(user_id);
        const std::string name = person ? person->value("name", user_id) : user_id;
        if (over_limit(user_limit, u, "user:" + user_id, name + "'s", name + " için", p, v)) return v;
    }
    if (!agent_limit.empty()) {
        auto ghost = store.agent(agent_id);
        const std::string name = ghost ? ghost->value("name", agent_id) : agent_id;
        if (over_limit(agent_limit, a, "agent:" + agent_id, name + "'s", name + " için", p, v)) return v;
    }
    return v;
}

namespace {
struct Row { long long tokens = 0, cached = 0, runs = 0, failed = 0; double cost = 0; };
void add_row(Row& w, const json& r) {
    w.tokens += run_tokens(r);
    w.cached += r.value("tokens_cached", 0LL);
    w.cost += run_cost(r);
    w.runs += 1;
    const std::string st = r.value("status", "");
    if (st == "failed" || st == "timeout") w.failed += 1;
}
json row_json(const Row& w) { return {{"tokens", w.tokens}, {"cached", w.cached}, {"cost", w.cost}, {"runs", w.runs}, {"failed", w.failed}}; }
json sorted_rows(const std::map<std::string, Row>& m) {
    std::vector<std::pair<std::string, Row>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.tokens != b.second.tokens ? a.second.tokens > b.second.tokens : a.first < b.first; });
    json out = json::array();
    for (auto& [id, w] : v) { json j = row_json(w); j["id"] = id; out.push_back(j); }
    return out;
}
}  // namespace

json usage_overview(Store& store, int days, const std::string& only_user) {
    days = std::clamp(days, 1, 366);
    Periods p = periods_now();
    std::tm first = local_tm((long long)std::time(nullptr));
    first.tm_hour = first.tm_min = first.tm_sec = 0;
    first.tm_mday -= days - 1;
    first.tm_isdst = -1;
    const long long since = (long long)std::mktime(&first);
    const long long stop_at = std::min(since, p.month);

    std::map<std::string, Row> by_day, by_agent, by_user, by_brain;
    Row today, month, period;
    std::map<std::string, Spend> user_spend, agent_spend;
    Spend company;
    char buf[16];
    for (auto& r : store.runs()) {
        const long long t = iso_to_unix(r.value("created", ""));
        if (t < stop_at) break;
        if (t >= p.month) {  // what the limits count: the whole company, also when one person looks at their own page
            const bool is_today = t >= p.day;
            add_spend(company, r, is_today);
            add_spend(user_spend[r.value("user_id", "")], r, is_today);
            add_spend(agent_spend[r.value("agent_id", "")], r, is_today);
        }
        if (!only_user.empty() && r.value("user_id", "") != only_user) continue;
        if (t >= p.day) add_row(today, r);
        if (t >= p.month) add_row(month, r);
        if (t < since) continue;
        add_row(period, r);
        std::tm lt = local_tm(t);
        strftime(buf, sizeof buf, "%Y-%m-%d", &lt);
        add_row(by_day[buf], r);
        add_row(by_agent[r.value("agent_id", "")], r);
        add_row(by_user[r.value("user_id", "")], r);
        std::string brain = r.value("provider", "");
        add_row(by_brain[brain.substr(0, brain.find_first_of("#@"))], r);
    }
    json day_rows = json::array();
    for (int i = 0; i < days; ++i) {
        std::tm d = first;
        d.tm_mday += i;
        d.tm_isdst = -1;
        std::mktime(&d);
        strftime(buf, sizeof buf, "%Y-%m-%d", &d);
        json j = row_json(by_day.count(buf) ? by_day[buf] : Row{});
        j["date"] = buf;
        day_rows.push_back(j);
    }
    json out = {{"days", days}, {"today", row_json(today)}, {"month", row_json(month)}, {"period", row_json(period)}, {"by_day", day_rows},
                {"by_agent", sorted_rows(by_agent)}, {"by_user", sorted_rows(by_user)}, {"by_brain", sorted_rows(by_brain)}};

    json b = store.budgets();
    const json users = b.value("users", json::object()), agents = b.value("agents", json::object());
    json limits = json::object();
    if (only_user.empty()) {
        json us = json::object(), as = json::object();
        for (auto& u : store.users()) {
            const std::string id = u.value("id", "");
            us[id] = {{"limit", users.contains(id) ? users[id] : json::object()}, {"used", spend_json(user_spend[id])}};
        }
        for (auto& a : store.agents()) {
            const std::string id = a.value("id", "");
            as[id] = {{"limit", agents.contains(id) ? agents[id] : json::object()}, {"used", spend_json(agent_spend[id])}};
        }
        limits = {{"company", {{"limit", b.value("company", json::object())}, {"used", spend_json(company)}}}, {"users", us}, {"agents", as}};
    } else {
        limits = {{"me", {{"limit", users.contains(only_user) ? users[only_user] : json::object()}, {"used", spend_json(user_spend[only_user])}}}};
    }
    out["budgets"] = limits;
    return out;
}

}  // namespace pc2
