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
    bool can_write() const;
    bool has_tool(const std::string& t) const;
};

AgentContext resolve_agent(Store& store, const json& agent, const std::string& user_role);

struct PromptInput {
    const AgentContext* ctx = nullptr;
    std::string user_name;       // the person this run works for
    bool shared_chat = false;    // several people may write in this conversation
    std::string workdir;         // absolute working folder
    std::string workdir_label;   // "project \"X\"" or "the shared files folder"
    json project;                // null when none
    bool name_chat = false;
};

std::string build_system_prompt(Store& store, const PromptInput& in);

struct SendBlock {
    std::string to;
    std::string title;
    std::string text;
    bool reply = true;
};
struct ParsedReply {
    std::string text;   // what is shown to people (blocks removed)
    std::string title;  // from <title>..</title>
    std::vector<SendBlock> sends;
};
ParsedReply parse_reply(const std::string& raw);
std::string clean_title(const std::string& s);  // one line, max 50 chars

}  // namespace pc2
