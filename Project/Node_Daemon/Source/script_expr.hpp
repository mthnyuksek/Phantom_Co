#pragma once
// Expressions of visual scripts: literals, variables, + - * / %, comparisons, and/or/not, abs/round/min/max.
// Parsed once, type-checked against the script's variables, evaluated at run time. See Development/Docs/visual_scripts.md.
#include <map>
#include <memory>
#include <set>
#include <string>

#include "script_value.hpp"

namespace pc2::script {

struct Node;

struct Expr {
    std::shared_ptr<const Node> root;
    std::string src;
};

using TypeEnv = std::map<std::string, Type>;
using ValueEnv = std::map<std::string, Value>;

// All three throw ScriptError (message + byte position in the expression when known).
Expr parse_expr(const std::string& src);
Type check_expr(const Expr& e, const TypeEnv& vars);
Value eval_expr(const Expr& e, const ValueEnv& vars);

// Names of the variables an expression reads.
std::set<std::string> expr_vars(const Expr& e);

// Test hook (--script-eval): one JSON request line in, one JSON answer line out.
// {"expr": "a + 1.50", "vars": {"a": {"type": "Number", "value": "2.00"}}}
// -> {"ok": true, "type": "Number", "value": 350, "text": "3.50", "text_tr": "3,50"}
// {"loose": {"type": "Time", "text": "16.00"}} tests parse_value_loose. Errors: {"ok": false, "stage", "error", "pos"}.
std::string script_eval_line(const std::string& line);

}  // namespace pc2::script
