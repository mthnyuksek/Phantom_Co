#pragma once
#include <string>
#include <vector>

#include "process.hpp"
#include "util.hpp"

namespace pc2 {

struct ProviderInfo {
    std::string id;     // "claude" | "codex" | "demo"
    std::string label;
    bool available = false;
    std::string path;
};

std::vector<ProviderInfo> detect_providers();
bool provider_known(const std::string& id);
std::string provider_exe(const std::string& id);  // full path of the claude / codex CLI, "" when not found

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
};

bool build_command(const RunSpec& rs, ProcessSpec& out, std::string& error);

// Codex's image tool saves into %CODEX_HOME% (`codex_home`, empty = the CODEX_HOME variable or ~\.codex)\generated_images\<session>\, outside the work folder. Copies the images of
// this session made since `since` into <workdir>\Generated_Images\ and returns their paths relative to the work folder.
std::vector<std::string> collect_codex_images(const std::string& session_id, fs::file_time_type since, const fs::path& workdir, const fs::path& codex_home = {});
std::string images_note(const std::vector<std::string>& images);  // "" or a line to add to the answer

// Turns the raw JSON lines of a CLI into the common event format:
//   {kind: status|message|thinking|tool_use|tool_result|log|error, text, ...}
class LineParser {
public:
    explicit LineParser(std::string provider) : provider_(std::move(provider)) {}
    std::vector<json> feed(const std::string& line);

    std::string result_text;  // final answer
    std::string session_id;   // to resume the conversation later
    bool is_error = false;
    bool max_turns = false;   // stopped because of the turn limit
    double cost_usd = -1;     // -1 = unknown
    long long tokens_in = 0, tokens_out = 0;
    json usage;

private:
    std::string provider_;
};

}  // namespace pc2
