# Roadmap

## Done (2026-11-01)
- Desktop program with its own window (WebView2), embedded local server, ghost UI (dark).
- Burrow's model: Markdown Vault, skills, rules, projects + notes, agents with bosses, chats with sessions, tasks as conversations, agent-to-agent hand-over with reports, runs with live view.
- Multi-user: accounts, roles, invitations, private/shared chats, agents act with the asker's rights, network sharing, connect to a friend's company from the window.
- Claude and Codex both verified end to end (including session resume). Upgrade of Vault version 1.

## Next
| # | Item | Why |
| --- | --- | --- |
| 1 | **Provider workers**: a host sends a job to another Phantom Co. that has the other CLI (Codex on the friend's PC, Claude on mine) and gets events/files back | The original goal: both CLIs usable by both people in one company |
| 2 | Wake-up scheduler (daily/weekly/every N minutes/one time) and the usage gate (5 h / 7 d Claude limits) | Port of Burrow features |
| 3 | Token budgets (per agent, per company) and an Overview page with per-agent/per-person usage | Port of Burrow features |
| 4 | Turkish / English interface (vault setting) | Burrow has it |
| 5 | Retry of failed runs, chat export, notifications (Telegram) | Port of Burrow features |
| 6 | Installer, app icon, light theme | Polish |
| 7 | HTTPS for sharing (or built-in relay) | Today: plain HTTP on a private network |

### About item 1
Today a company uses the host's providers only. Design sketch: each Phantom Co. can be a *worker*: the host adds a worker by invitation (mutual key pinning); the agent's `provider` can be `codex@friend`; the host sends `{job, prompt, session}` and receives the event stream; files are exchanged through the project folder (the worker mirrors it, or the job uses the host's folder over a share). Needs a protocol (`Project/Protocol`) and a sync or share story for project folders.

## Open questions
- Should friends' own companies be listed (switch from the sidebar) instead of the "home" button?
- Where should large files live (UE assets, video) once projects are shared between PCs?
