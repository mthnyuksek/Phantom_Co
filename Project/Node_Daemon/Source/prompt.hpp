#pragma once
#include <string>
#include <vector>

#include "store.hpp"

namespace pc2 {

int role_rank(const std::string& role);  // viewer 0, editor 1, owner 2, unknown -1

// What an agent may do in one run. Agents act with the rights of the person who
// started the run: skills that need a higher role than that person's are left out.
struct AgentContext {
    json agent;
    std::vector<json> skills;           // effective skills
    std::vector<std::string> tools;     // union of their tools (Claude tool names)
    std::vector<std::string> add_dirs;  // extra Vault folders the skills may touch (absolute)
    std::vector<std::string> mcp;       // MCP servers the skills open (names; the runner keeps those that exist and are on)
    std::vector<std::string> read_dirs;   // folders outside the Vault the run's project lets the ghost read (absolute; set by the runner)
    std::vector<std::string> write_dirs;  // ... and change (only when the ghost has a writing tool)
    bool can_write() const;
    bool has_tool(const std::string& t) const;
};

// Skills with a project list are used only in those projects (project_id "" = shared files).
bool skill_fits_project(const json& skill, const std::string& project_id);
AgentContext resolve_agent(Store& store, const json& agent, const std::string& user_role, const std::string& project_id);
// Adds the project's extra folders (`folders`: [{path, mode: read|write}]) to ctx.read_dirs / ctx.write_dirs. Only for a ghost with
// a file or shell tool; folders that no longer exist are skipped; "write" needs a writing tool, else the folder is read only.
void apply_project_folders(AgentContext& ctx, const json& project);

struct PromptInput {
    const AgentContext* ctx = nullptr;
    std::string user_name;       // the person this run works for
    bool shared_chat = false;    // several people may write in this conversation
    std::string workdir;         // absolute working folder
    std::string workdir_label;   // "project \"X\"" or "the shared files folder"
    json project;                // null when none
    bool name_chat = false;
    bool memory_on = false;      // the ghost keeps notes between chats (its notes about this person follow)
    std::vector<std::string> memory;
};

std::string build_system_prompt(Store& store, const PromptInput& in);

struct SendBlock {
    std::string to;
    std::string title;
    std::string text;
    bool reply = true;
};
// <script name="..." id="...">{json}</script>: a visual script written by a ghost (saved as a draft, see Runner::handle_script)
struct ScriptBlock {
    std::string name;
    std::string id;    // set = change this draft instead of making a new script
    std::string body;  // the JSON (code fences removed)
};
struct ParsedReply {
    std::string text;   // what is shown to people (blocks removed)
    std::string title;  // from <title>..</title>
    std::vector<SendBlock> sends;
    std::vector<ScriptBlock> scripts;
    std::vector<std::string> remember;  // <remember>a short fact</remember>
    std::vector<int> forget;            // <forget>note number</forget>
};
// memory: take <remember> and <forget> out of the answer (ghosts with memory only; for others they stay plain text).
ParsedReply parse_reply(const std::string& raw, bool memory = false);
std::string clean_title(const std::string& s);  // one line, max 50 chars

}  // namespace pc2
