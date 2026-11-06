#pragma once
#include <string>
#include <vector>

#include "process.hpp"
#include "util.hpp"

namespace pc2 {

struct ProviderInfo {
    std::string id;     // "claude" | "codex" | "gemini" | "opencode" | "demo"
    std::string label;
    bool available = false;
    std::string path;
};

std::vector<ProviderInfo> detect_providers();
bool provider_known(const std::string& id);
std::string provider_exe(const std::string& id);  // full path of the claude / codex / gemini / opencode CLI, "" when not found

struct RunSpec {
    std::string provider;
    std::string model;
    std::string workdir;
    std::string system_prompt;
    std::string system_file;               // where the Claude system prompt is written (Claude only)
    std::string prompt;                    // user message, sent through stdin
    std::vector<std::string> tools;        // Claude tool names granted by skills
    std::vector<std::string> add_dirs;     // extra folders the run may touch
    std::string session_id;                // resume this session ("" = new)
    int max_turns = 40;
    bool can_write = false;                // maps to the Codex sandbox mode
    std::vector<std::pair<std::string, std::string>> env;  // account of this run (CLAUDE_CONFIG_DIR / CODEX_HOME), empty = Auto
    // MCP servers of this run: each one is "<this exe> --mcp-proxy <name> --mcp-port <port> --mcp-key <key>" (see mcp.hpp).
    std::vector<std::string> mcp;          // server names
    std::string mcp_exe, mcp_key;
    int mcp_port = 0;
    std::string mcp_file;                  // Claude: --mcp-config file; Gemini: system settings file; OpenCode: the run's whole config (always; written by build_command)
};

bool build_command(const RunSpec& rs, ProcessSpec& out, std::string& error);

// Codex's image tool saves into %CODEX_HOME% (`codex_home`, empty = the CODEX_HOME variable or ~\.codex)\generated_images\<session>\, outside the work folder. Copies the images of
// this session made since `since` into <workdir>\Generated_Images\ and returns their paths relative to the work folder.
std::vector<std::string> collect_codex_images(const std::string& session_id, fs::file_time_type since, const fs::path& workdir, const fs::path& codex_home = {});
std::string images_note(const std::vector<std::string>& images);  // "" or a line to add to the answer
// Codex's home folder: `codex_home` (an account), else the CODEX_HOME variable, else ~\.codex.
fs::path codex_home_dir(const fs::path& codex_home = {});
// The last "rate_limits" object Codex wrote into the session file of `session_id` (sessions\YYYY\MM\DD\rollout-*-<id>.jsonl,
// looked for in the last 3 days), null when not found. Codex's exec --json does not print it.
json codex_rate_limits(const std::string& session_id, const fs::path& codex_home = {});

// Turns the raw JSON lines of a CLI into the common event format:
//   {kind: status|message|thinking|tool_use|tool_result|log|error, text, ...}
// Used for Gemini ghosts without a model: works with a free API key (Pro models have no free quota).
inline constexpr const char* kGeminiDefaultModel = "gemini-flash-lite-latest";

// OpenCode ("other models"): one CLI for many model services. An API key account names its service (picked in Settings,
// or found from the key's prefix); a ghost without a model uses the service's default model (OpenCode names models
// "<service>/<model>"). Local model servers (LM Studio, Ollama) need no account: the ghost's Model field says
// "lmstudio/<model>" or "ollama/<model>" and the run's config points OpenCode at the server on this PC.
struct OpenCodeService {
    const char* id;          // OpenCode's provider id, e.g. "nvidia"
    const char* label;       // "NVIDIA"
    const char* prefix;      // key prefix, e.g. "nvapi-" ("" = the service's keys have no telling prefix)
    const char* env;         // environment variable OpenCode reads the key from
    const char* model;       // default model
    const char* check;       // key check: GET this URL with "Authorization: Bearer <key>" (401/403 = rejected); "" = NVIDIA's 1-token chat
    const char* models;      // public model list (OpenAI style, no key needed) for the editor and short names; "" = none
};
const OpenCodeService* opencode_service_for_key(const std::string& key);  // nullptr = no service has this key's prefix
const OpenCodeService* opencode_service(const std::string& id);
const std::vector<OpenCodeService>& opencode_services();
// Chat models of a service's public list as "<service>/<model>" (kept 10 minutes; empty when the list cannot be read).
std::vector<std::string> opencode_service_models(const OpenCodeService& s);
// "nvidia/minimax-m3.1-flash" -> "nvidia/minimaxai/minimax-m3.1-flash": a model name without its maker is looked up in the
// service's list (one match only). Anything else comes back unchanged.
std::string opencode_full_model(const std::string& model);

struct OpenCodeLocal {
    const char* id;          // model prefix and OpenCode provider id: "lmstudio" | "ollama"
    const char* label;       // "LM Studio"
    const char* host;        // "http://127.0.0.1:1234"
};
const std::vector<OpenCodeLocal>& opencode_locals();
// Models the local servers of this PC offer now ("lmstudio/<id>"...), asked with a short timeout; servers that are off are skipped.
json opencode_local_models();

class LineParser {
public:
    explicit LineParser(std::string provider) : provider_(std::move(provider)) {}
    std::vector<json> feed(const std::string& line);

    std::string result_text;  // final answer
    std::string session_id;   // to resume the conversation later
    bool is_error = false;
    bool max_turns = false;   // stopped because of the turn limit
    bool stop_now = false;    // the run cannot succeed (Gemini: quota used up, Google sign-in refused): kill the CLI
    double cost_usd = -1;     // -1 = unknown
    long long tokens_in = 0, tokens_out = 0;
    json usage;
    json rate_limits;         // Claude: the last rate_limit_info (subscription usage windows), null when none came

private:
    std::vector<json> feed_gemini(const json& j, const std::string& type);
    std::vector<json> feed_opencode(const json& j, const std::string& type);
    std::string gemini_error(const std::string& raw) const;  // readable error; a used-up quota becomes one clear text
    std::string provider_;
    std::string pending_;  // Gemini / OpenCode: assistant text since the last tool call
    bool after_tool_ = false;  // OpenCode: a tool ran since the last text
    std::string model_;    // Gemini: the model named by the init event
    int quota_hits_ = 0;   // Gemini: "Attempt n failed with status 429" lines seen
};

}  // namespace pc2
