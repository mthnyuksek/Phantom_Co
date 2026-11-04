# Phantom Co. Vault

This folder was created by the Phantom Co. program. It is the work folder of your ghost company. You can open and edit everything here with any tool (the program re-reads the files).

| Folder / file | What is inside |
| --- | --- |
| `Company/company.json` | Company name and goal. |
| `Users/` | One `<id>.md` per person (name and look). Passwords and roles are NOT here. |
| `Agents/` | One `<id>.md` per ghost: name, role, boss, brain, look, skills; the text below the header is the persona. |
| `Skills/` | One `<id>.md` per skill: tools and how to use them. Agents get tools only through skills. A `projects:` line limits a skill to those projects. |
| `Rules/` | Company rules added to every agent's prompt. |
| `Projects/<id>/` | A project's work folder. The hidden `.Phantom` folder holds its summary, notes and each ghost's job in the project (`Roles/`). |
| `Tasks/` | One `<id>.md` per task. |
| `Task_Templates/` | One `<id>.md` per ready-made task (title, details, ghost, project, wake-up). |
| `Conversations/` | One `<id>.jsonl` per chat, task or agent-to-agent conversation. |
| `Runs/` | One record (`.md`) and one event log (`.jsonl`) per run. |
| `Shared_Files/` | Work that belongs to no project. |
| `vault.json` | Marker file: Vault version, id and settings. Do not delete. |

Agents can work without asking for approval inside their working folder (a project or `Shared_Files`) and only with the tools their skills give them.
