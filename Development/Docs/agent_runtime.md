# Agent runtime

How the program runs a provider CLI for one job. Verified on 2026-11-01 with Claude Code 2.1.316 and codex-cli 0.166.0 (Windows), including session resume.

## Common
- Prompts go through **stdin** (no quoting problems, no length limit); the pipe is closed after writing (Codex waits for it).
- The CLI runs in a Job Object: stop/timeout/closing the program kills the whole process tree.
- Working folder: the project folder (`Projects/<id>`) or `Shared_Files`.
- The first message of a conversation carries a note asking for `<title>2-4 words</title>`; the router strips it and renames the chat.
- Output is turned into common events (`status, message, thinking, tool_use, tool_result, log, error, done`) stored in `Runs/<id>.jsonl`.
- Tokens: Claude `usage` (input + cache + output) and `total_cost_usd`; Codex `turn.completed.usage`. Stored in the run record.

## Claude (lean profile, the same idea as Burrow)
```
claude -p --output-format stream-json --verbose --system-prompt-file <tmp>
       --tools "<skill tools>" [--allowedTools "<same>"] --strict-mcp-config --disable-slash-commands
       --setting-sources project --permission-prompts none --max-turns 40|25
       [--model m] [--resume <session>] [--add-dir <vault folder>...]
```
Only the tools granted by skills exist (`--tools ""` = none), nothing is asked interactively (anything not allowed is denied), no MCP servers, no slash commands. A call with no tools is about 0.7k tokens, with Read+Write about 4.7k (measured with haiku, cost about $0.001). `max_turns` ends with `error_max_turns`; the session is kept and the chat shows "write continue".

## Codex
```
codex exec --json --skip-git-repo-check -s read-only|workspace-write -C <folder> [--add-dir ..] [-m m] -
codex exec resume <thread> --json --skip-git-repo-check -c sandbox_mode="..." [-m m] -
```
Codex has no system prompt option: the persona, rules and skill texts travel in front of the message (resent on every run). Its shell is sandboxed: in the real test it was refused writing to `C:\` and then wrote into the working folder. Skill tools only decide the sandbox mode (any of Write/Edit/Bash/PowerShell -> `workspace-write`).

## Which tools a run gets
Skills of the agent that are switched on **and** allowed for the role of the asking person (`min_role`). The union of their `tools` is the tool list; their `dirs` become `--add-dir` (Claude/Codex first run). See `multi_user.md`.

## Limits
`timeout_min` per agent (default 30), `max_turns`, `max_concurrent` runs per provider (setting, default 1), an agent never runs twice at once, `max_queue_per_user`, `max_hops` for `<send>` chains. The vault-wide `default_model` is only used for Claude.

## Remote provider (a friend's PC)
`provider: claude@<user>` / `codex@<user>`. The runner builds the same prompt, tools and session as for a local run, but the working folder is written as `@@WORKDIR@@` (the friend's program fills in its mirror folder), tools are cut to `Read Write Edit Glob Grep WebSearch WebFetch`, there are no `--add-dir`s, and the job goes to `WorkerHub`. Events from the friend are relayed into the run log; the result carries text, session, tokens and cost. Sessions are stored as `@<user>:<id>` and only resumed on that PC. One remote lane (one job at a time) per friend. Timeouts: 90 s until the friend picks the job up, 120 s of silence while running (the worker sends a heartbeat every 15 s), the agent's `timeout_min` for the run.

## Demo provider
No process: a scripted agent for trying the app and for tests. Keywords in the message steer it: `demo:send:<agent_id>` (hands work over), `demo:write` (writes a file if it has the Write tool), `demo:slow` (waits, for stop tests), `demo:fail`.
