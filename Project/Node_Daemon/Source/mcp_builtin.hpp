#pragma once
#include <string>

#include "util.hpp"

namespace pc2::builtin {

// MCP servers built into phantom_co.exe (no Node or Python needed): "telegram" (a bot) and "youtube" (your channel).
// `session` is what Mcp::session returns: secrets, options and the run's work folder (the only place files are taken from).

json tools(const std::string& builtin);  // the MCP tool list
// Runs one tool: an MCP CallToolResult {content: [{type: text, text}], isError}.
json call(const std::string& builtin, const std::string& tool, const json& args, const json& session);
// Settings > MCP: does it work? {ok, text} (Telegram: the bot's name, YouTube: the channel's name).
json check(const std::string& builtin, const json& secrets, const json& options);

// YouTube sign-in (OAuth for installed apps, loopback redirect, PKCE).
std::string google_auth_url(const std::string& client_id, const std::string& redirect, const std::string& state, const std::string& challenge);
std::string pkce_challenge(const std::string& verifier);
bool google_exchange(const std::string& client_id, const std::string& client_secret, const std::string& code, const std::string& verifier,
                     const std::string& redirect, std::string& refresh_token, std::string& error);

}  // namespace pc2::builtin
