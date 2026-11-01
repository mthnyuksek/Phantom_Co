# Phantom Co.: project summary

Read this instead of scanning the source. Details per file: `file_structure.md`. Keep this file current.

## What it is
A Paperclip-like "ghost company": people chat with agents, give them tasks and projects, agents hand work to each other. Built on the same ideas as the Burrow project (skills, projects with notes, agents, chats with a reply stack, lean prompts) but **multi-user** and with **Claude and Codex** as brains.
It is a normal Windows program, **`phantom_co.exe`** (GUI subsystem, one static file): a Win32 window that hosts the web interface in WebView2. Inside the same process runs a small HTTP server (cpp-httplib) on `127.0.0.1` that serves the UI and the API; the window just shows `http://127.0.0.1:<port>/`. When the owner switches sharing on, the server listens on the network so friends' PC2 windows can connect (they see the owner's company; the work runs on the owner's PC with the owner's CLI sign-ins).
C++20, CMake + Ninja + MinGW. Libraries (single headers in `Third_Party`): cpp-httplib 0.26, nlohmann/json 3.11. WebView2 SDK headers + loader DLL (from Burrow). Windows only.

## Build, run, test
See `Docs/running.md`. Short: `cmake -S Project/Node_Daemon -B Project/Node_Daemon/Build -G Ninja -DCMAKE_BUILD_TYPE=Release`, `cmake --build ...`, `Build\phantom_co.exe`. Options: `--vault`, `--port`, `--share`, `--data`, `--no-window` (server only, used by tests), `--devtools`, `--ui-dir <Project\Web_Ui>` (read the UI from disk, no rebuild for UI edits).
Test: `python -X utf8 -I Project/Node_Daemon/Tests/smoke_test.py <phantom_co.exe> [--real claude|codex]` (about 120 checks, isolated temp Vault and data folder, port 7791).
UI testing in the real window: set env `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223 --remote-allow-origins=*`, then drive it with a CDP client (Node 24 has a global WebSocket).

## Layers (Source/)
- `util` ids (ULID), time, UTF-8 <-> wide, atomic file writes, json helpers. `md` Markdown files with a `---` header (`key: value`, repeated keys = lists).
- `store` the Vault (all entities as Markdown files, conversations as jsonl, run index in memory), v1 -> v2 upgrade, node config (`%LOCALAPPDATA%\PC2\node.json`).
- `auth` accounts, roles, password hashes (PBKDF2-SHA256 via BCrypt), sessions, invites, login throttling. Stored in `%LOCALAPPDATA%\PC2\auth.json`, **never in the Vault** (agents with file tools must not read hashes or change roles).
- `prompt` effective skills for a user role, lean system prompt, `<send>` / `<title>` parsing.
- `providers` command lines for `claude` and `codex`, output parsers (events, session id, tokens, cost). `process` Win32 child process in a Job Object (kills the whole tree).
- `runner` job queue (lanes per provider, `max_concurrent`, an agent never runs twice at once), run records, live events, the **router** (writes answers to conversations, forwards `<send>`, hands reports back along the reply stack). Built-in `demo` provider for tests/trying.
- `server` REST API + static UI, auth gate, role checks, sharing switch (restarts the listener). `web_view_host` WebView2 wrapper. `main` window, server thread, single-instance mutex.
- `Web_Ui/` plain ES modules, no build step: `app.js` shell/auth/router, `pages/*` one module per tab, `chat_view.js` conversation component, `agent_editor.js`, `pickers.js`, `ghosts.js` (SVG ghosts), `state.js`, `ui.js`, `api.js`.

## Vault (version 2)
See `Docs/vault_format.md`. `vault.json` (version, id, settings), `Company/company.json`, `Users/<id>.md` (profile only), `Agents/`, `Skills/`, `Rules/` (`<id>.md`), `Projects/<id>/` (the work folder) with hidden `.Pc2/project.md` + `.Pc2/Notes/`, `Tasks/<id>.md`, `Conversations/<id>.jsonl`, `Runs/<id>.md` + `.jsonl`, `Shared_Files/`.
Default Vault: `<Documents>\PC2_Vault`, created by the program. A version 1 Vault (JSON agents) is upgraded on first open: agents become Markdown files with skills derived from the old access level, `Archive_V1/` keeps the old files.

## How a run works
1. A chat message / task starts a `Job` with a reply stack `[{user, conv}]`. `Runner::enqueue` writes `Runs/<id>.md` (queued) and queues it on the provider lane of the agent.
2. `execute`: resolve skills for the **asking user's role** (skills with `min_role: owner` are dropped for editors), build the system prompt (identity, where you are, company rules, persona, skills, project summary + notes, team list + `<send>` protocol), working folder = project folder or `Shared_Files`, resume the session stored in the conversation. Claude: `-p --output-format stream-json --system-prompt-file --tools <skill tools> --strict-mcp-config --disable-slash-commands --setting-sources project --permission-prompts none --max-turns --resume --add-dir`. Codex: `exec [resume <id>] --json -s read-only|workspace-write`, persona in front of the message.
3. `route`: answer -> conversation of the top frame (`<title>` renames, `<send>` blocks create an agent conversation and a child job with the stack extended by `{agent, sender, conv, notify}`); when a job whose top frame is an agent finishes, the answer is also handed back as a `report` job to that agent (stack minus the top frame). Failures are reported back too, so chains never hang. `max_hops` is the safety net; a duplicate request to an agent that is already being asked is dropped.
4. A conversation is "busy" while any active job has it anywhere in its stack (the chat composer is locked).

## Multi-user rules
Roles: owner (everything), editor (chat, tasks, projects, hire agents), viewer (read only). Chats are private to their creator unless shared; task conversations are team-wide; agent-to-agent conversations follow the chat they came from; the owner sees everything. **Agents act with the rights of the person who asked** (`min_role` of skills). Powerful skills (commands, Vault folders other than Projects) are forced to `min_role: owner`; only the owner edits skills and rules. Per-user queue limit (`max_queue_per_user`).
Sessions: HttpOnly cookie `pc2_session_<port>` (the port is in the name because cookies are shared between ports of one host), 30 days. API calls need `X-PC2: 1` and a same-origin `Origin`; the Host header must be local unless sharing is on (sign-in is always required).

## Pitfalls (already hit once)
- `GetLastError()` after `CreateMutexW` can still hold `ERROR_ALREADY_EXISTS` from earlier calls: `SetLastError(0)` first.
- Headless mode must wait for the server thread (`join`), not stop it.
- A model name from the vault-wide `default_model` is a Claude name: never pass it to Codex.
- Bash/Python heredocs run through the tool eat backslashes (`\n`, `\\`): use the Write/Edit tools for source files.
- WebView2 on MinGW: `__uuidof` does not work, IIDs are copied by hand in `web_view_host.cpp`; callback objects answer only `IUnknown`.
- httplib: do security checks in `set_pre_request_handler` (after the body is read), not in the pre-routing handler (the client sees a reset connection).
- Cookies are per host, not per port (see above).
- Prompts go through stdin; Codex waits for stdin to close, so the pipe is always closed after writing.
- `windows.h` needs `WIN32_LEAN_AND_MEAN` + `<winsock2.h>` first when `<iphlpapi.h>` is used.

## Not done yet
Remote providers (a company using the Claude of one PC and the Codex of another at the same time), wake-up scheduler, usage gate and token budgets, overview/statistics page, Telegram, HTTPS, Turkish/English UI, installer and icon, light theme. See `Docs/roadmap.md`.

## Naming
The product is called **Phantom Co.** (exe `phantom_co.exe`, CMake target `phantom_co`). The folder `Projects/PC2` and these internal identifiers still say "pc2" on purpose, so existing Vaults and data keep working: C++ namespace `pc2`, header `X-PC2`, cookie `pc2_session_<port>`, hidden `.Pc2` project folder, `%LOCALAPPDATA%\PC2`, default Vault `Documents\PC2_Vault`, localStorage key and `pc2:*` window events.
