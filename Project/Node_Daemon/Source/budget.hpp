#pragma once
// Spending limits and the numbers of the Overview page. Limits live in the Vault (Company/budgets.json):
//   {"company": L, "users": {id: L}, "agents": {id: L}}, L = {day_tokens, month_tokens, day_usd, month_usd}, 0 or missing = no limit.
// Days and months are this PC's calendar. Tokens = in + out as the Runs page shows them; dollars = what the CLI reported
// (subscription runs report what the run would have cost). A run already going is never stopped by a limit.
#include "store.hpp"

namespace pc2 {

struct BudgetVerdict {
    bool blocked = false;
    std::string text, text_tr;  // why ("Budget reached: ...")
    std::string key;            // scope + period, so a notice is sent once per period
};

inline bool is_budget_error(const std::string& s) { return s.rfind("Budget reached", 0) == 0; }

long long iso_to_unix(const std::string& iso);  // "2026-11-08T10:00:00Z"; 0 = not a time
json clean_budgets(const json& in, Store& store);
BudgetVerdict budget_check(Store& store, const std::string& agent_id, const std::string& user_id);
// The numbers of the Overview page; only_user not empty = that person's runs only and no company-wide limits.
json usage_overview(Store& store, int days, const std::string& only_user);

}  // namespace pc2
