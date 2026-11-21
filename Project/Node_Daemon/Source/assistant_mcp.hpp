#pragma once
#include <functional>
#include <string>

#include "util.hpp"

namespace pc2 {

// MCP server for the person's own AI assistant (Streamable HTTP at POST /mcp, signed in with an API key).
// Every tool is a call to Phantom Co.'s own REST API with the same key, so the key's rights and the
// usual checks apply unchanged; this file only turns tool calls into API calls and answers into text.
struct ApiResult {
    int status = 0;
    json body;
};
struct AssistantApi {
    std::function<ApiResult(const std::string& method, const std::string& path, const json& body)> call;  // body null = none
    std::function<bool(const std::string& conv_id)> busy;  // is a ghost still working in this conversation?
    std::string script_format;  // how a visual script looks (the visual_scripts skill)
    std::string version;
};

// One JSON-RPC message (or a batch). Returns the answer, or null when nothing is answered (notifications).
json assistant_mcp(const json& msg, const AssistantApi& api);

}  // namespace pc2
