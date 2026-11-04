# Vault format (version 2)

The Vault is a folder of plain files you can open with any editor (Obsidian style). The program creates it on first start (`<Documents>\Phantom_Vault`, visible on purpose). Markdown entities are `---` header lines (`key: value`; a repeated key is a list) followed by free text.

```
<vault>/
  vault.json                    {vault_version: 2, vault_id, created, settings: {max_hops, default_model, max_queue_per_user, max_concurrent}}
  README.md                     explains the folder to people who open it
  Company/company.json          {name, goal}
  Users/<id>.md                 name, form, color, created        (profile only; role and password live in %LOCALAPPDATA%\Phantom_Co\auth.json)
  Agents/<id>.md                name, role, reports_to, provider, model, form, color, enabled, timeout_min, skill: <id> (repeat), created; body = persona
  Skills/<id>.md                name, description, tools: Read, Glob, ..., dirs: Projects, min_role: editor|owner, enabled, projects: a, b (empty = everywhere; else only in those projects); body = how to use it
  Rules/<id>.md                 name, enabled; body = the rule (added to every agent's prompt)
  Projects/<id>/                the project's work folder; agents run here
    .Phantom/project.md             name, created; body = summary        (hidden folder)
    .Phantom/Notes/<id>.md          name, enabled; body = the note       (shared memory)
    .Phantom/Roles/<agent id>.md    name; body = that agent's job in this project (added to its prompt only here)
  Tasks/<id>.md                 name, agent, project, created_by, created, updated, conversation, wake, wake_every, wake_at, wake_on, wake_last; body = description
  Task_Templates/<id>.md        name, agent (may be empty), project, created_by, created, wake, wake_every, wake_at; body = task details
  Conversations/<id>.jsonl      one JSON object per line (see below)
  Runs/<id>.md                  kind, task, agent, user, provider, model, conversation, status, created, started, ended, exit_code, tokens_in, tokens_out, cost, error; body = result
  Runs/<id>.jsonl               event log of the run (message, tool_use, tool_result, thinking, error, done)
  Shared_Files/                 work that belongs to no project
  Archive_V1/                   only after an upgrade from version 1
```

Ids are file names: lower case words joined with `_` (`luna`, `moon_base`); a clash gets a number. Tasks, conversations and runs use ULIDs (time sortable, no clashes between machines).

## Conversation lines
```
{"t":"meta","kind":"user|task|agents","title":..,"auto_title":true,"agent_id":..,"user_id":..,"project_id":..,"task_id":..,"shared":false,"participants":[..],"parent":<conv>,"created":..}
{"t":"msg","from":"user:<id>|agent:<id>|system","text":..,"run":<run id>,"ts":..}
{"t":"title","title":..}   {"t":"share","shared":true}   {"t":"project","project_id":..}
{"t":"session","agent":<id>,"sid":<CLI session id>}      the session to resume next time
{"t":"reset","text":..}                                   forget all sessions of this conversation
```
Later lines win (title, share, project, session). A task has one conversation (`kind: task`, shared with the team); the task description is its first message and every "run again" adds to it.

## Skills and tools
Tool names are Claude Code's: Read, Glob, Grep, Write, Edit, Bash, PowerShell, WebSearch, WebFetch. For Codex a skill set that contains any of Write, Edit, Bash, PowerShell means sandbox `workspace-write`, otherwise `read-only`. `dirs` (Agents, Skills, Rules, Projects) are extra Vault folders the run may touch (`--add-dir`); nothing else of the Vault (conversations, runs, users) is ever added. Skill templates are built into the program (`Project/Skill_Templates`); new Vaults get the ones marked `default: true`.

## Upgrade from version 1
Version 1 stored agents as `Agents/<id>.json` with an `access` level and tasks as folders. On first open: default skills are installed, each agent becomes an `Agents/<id>.md` (access `read_only` -> skills `read_files`; `workspace` -> + `write_files`; `workspace_shell` -> + `shell_commands`), old files move to `Archive_V1/`, `vault.json` gets version 2. Nothing is deleted.
