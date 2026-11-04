# Architecture

## One process

```
 phantom_co.exe
 ├─ main thread      Win32 window + WebView2  ──shows──>  http://127.0.0.1:<port>/
 ├─ server thread    cpp-httplib: UI files, REST API, auth gate        (restarts when sharing changes)
 ├─ worker threads   4 per provider lane (claude, codex, demo); at most max_concurrent run
 └─ per run          watchdog thread (time limit) + child process in a Job Object (claude / codex CLI)
```
Shared objects: `Store` (Vault files, recursive mutex), `Auth` (`%LOCALAPPDATA%\Phantom_Co\auth.json`), `Runner` (jobs, live state).

## Data flow of a chat message
1. Page -> `POST /api/chats/<id>/send` (cookie session, role check, visibility check, conversation not busy).
2. Server appends the user message to `Conversations/<id>.jsonl`, builds a `Job` (reply stack `[user@conv]`) and calls `Runner::enqueue`: a run record `Runs/<run>.md` (queued) and a lane queue entry.
3. A worker picks the job (agent not already running), `execute` resolves skills for the asker's role, builds the system prompt and the command, starts the CLI (prompt through stdin), reads stream-json / JSONL lines into events (`Runs/<run>.jsonl`, memory, live activity lines) and the final text.
4. `route` writes the answer, title, session id and system lines to the conversation, creates child jobs for `<send>` blocks, hands reports back along the stack, then the job is marked finished (so the conversation stays "busy" until the whole chain is done).
5. The page polls `GET /api/chats/<id>` (0.8 s while busy, 3 s otherwise) and `GET /api/state` (2 s); no WebSockets.

## Window and pages
`main.cpp` creates the window, `web_view_host` the WebView2 control (loader DLL unpacked from the executable into `%LOCALAPPDATA%\Phantom_Co\runtime`, profile in `webview_data`). Navigation is limited to allowed origins (local address, plus origins the program opened itself); everything else opens in the normal browser. Messages from the page: `connect` (open another company) and `home`.

## Security gate (server)
Host header must be local unless sharing is on; non-GET needs `X-PC2: 1` and a same-origin Origin; every API route except sign-in/join/status/static files needs a session and a minimum role (`add(method, pattern, min_role, handler)` in `server.cpp`). See `security.md`, `multi_user.md`.

## Files of the program
See `../file_structure.md`. Embedded at build time by `Cmake/embed.cmake`: `Web_Ui` (group `ui`), `Vault_Template` (`vault`), `Skill_Templates` (`skills`), the WebView2 loader (`loader`).
