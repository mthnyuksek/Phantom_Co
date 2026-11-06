#pragma once
// Script files (Vault Scripts/<id>.json): cleaning what the editor or an agent sends, and checking a script before it runs.
// Format: Development/Docs/visual_scripts.md.
#include <functional>
#include <optional>
#include <set>
#include <string>

#include "script_expr.hpp"

namespace pc2::script {

constexpr size_t kMaxNodes = 200, kMaxEdges = 400, kMaxVars = 100;

// Read-only variables every script has (filled by the script runner at run time).
const TypeEnv& builtin_types();

// Keeps only known fields with sane sizes, on top of `base` (the stored script or {} for a new one).
// Fails (err) only for input that cannot be a script at all (not an object, too many nodes, bad ids...):
// a half-finished script is still saved, check_script() lists its problems.
std::optional<json> clean_script(const json& in, json base, std::string& err);

struct ScriptLookup {
    std::function<bool(const std::string&)> agent_exists;
    std::function<bool(const std::string&)> project_exists;
};

// {"errors": [{"message", "node"?, "edge"?, "var"?, "field"?, "pos"?}], "prompt_count": n}
// A script runs only without errors.
json check_script(const json& s, const ScriptLookup& lookup);

// Do N: the blocks reached from its loop output (they run again every round).
std::set<std::string> loop_body(const json& script, const std::string& do_id);

// The {name} placeholders of a prompt text (only names that look like variables).
std::vector<std::string> text_placeholders(const std::string& text);

}  // namespace pc2::script
