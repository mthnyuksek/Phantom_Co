# 0006 – Provider workers: a friend lends Claude/Codex to the host

- **Status**: Accepted (2026-11-01). First version of roadmap item 1.
- **Context**: Agents ran only with the CLIs of the host's PC. Friends should be able to lend their own Claude Code / Codex (their own subscription) to the host's company, and everybody should be able to exchange files with the host's Vault.
- **Decision**:
  - **Pull model.** The friend's program opens the connection (`POST /api/worker/poll`, long poll of 20 s) with a *worker token* (Bearer, one per user, stored hashed in `auth.json`). No port has to be opened on the friend's PC and the friend's NAT does not matter. The host (`WorkerHub`) queues jobs per friend.
  - An agent's `provider` can be `claude@<user id>` / `codex@<user id>`. The runner builds the run exactly as for a local one (skills of the asking person's role, system prompt, session) and, instead of starting a process, hands `{provider, model, system prompt, prompt, tools, session, scope}` to the hub and relays the events. One lane (one job at a time) per friend.
  - **Files**: the run's folder (`shared` or `project:<id>`) is mirrored on the friend's PC (`%LOCALAPPDATA%\Phantom_Co\Worker\<host>\<scope>`): SHA-256 manifest down before the run, new/changed files up after it (50 MB per file, 5000 files; deletions are not sent back). Last writer wins.
  - **Sessions** live on the PC that ran them: stored as `@<user>:<id>`; a session is only resumed on the same PC.
  - **Safety for the friend**: the friend's side only starts `claude`/`codex`, with the tools `Read Write Edit Glob Grep WebSearch WebFetch` (the host strips Bash/PowerShell and extra folders, the worker filters again), validates model/session strings (they end up in a command line), works in its mirror folder. It still runs prompts written by the host, so the friend switches it on knowingly (profile → "Lend my AI") and only for a company they trust.
  - File exchange for people: upload/download/new folder in the Files page (editors upload, everybody downloads, owner deletes).
- **Not done**: automatic sync of huge folders (UE assets), propagating deletions, running a friend's agent when the friend's PC is offline (the run fails after 90 s), HTTPS.
- **Consequences**: new files `worker_hub.*` (host) and `worker.*` (friend); window messages `worker_start|worker_stop|worker_status`; `node.json` may hold `worker_url`/`worker_token` (the lending resumes at the next start).
