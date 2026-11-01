# Roadmap

## Done (2026-11-01)
- Desktop program with its own window (WebView2), embedded local server, ghost UI (dark).
- Burrow's model: Markdown Vault, skills, rules, projects + notes, agents with bosses, chats with sessions, tasks as conversations, agent-to-agent hand-over with reports, runs with live view.
- Multi-user: accounts, roles, invitations, private/shared chats, agents act with the asker's rights, network sharing, connect to a friend's company from the window.
- Claude and Codex both verified end to end (including session resume). Upgrade of Vault version 1.

- Provider workers, first version (decision 0006): friends lend their Claude/Codex (profile → "Lend my AI"), agents can use `claude@<friend>`, the run folder is mirrored to the friend's PC and back. File upload/download in the Files page. Extra owner accounts from the sign-in screen (local only).

- Internet access through a Cloudflare quick tunnel (friends outside the network), https for the friend's program.

## Next
| # | Item | Why |
| --- | --- | --- |
| 1 | Provider workers, next steps: send deletions back, big folders (UE assets) without full mirroring, queue a job until the friend's PC comes online, a usage gate per lent AI | The first version works (see Done) |
| 2 | Wake-up scheduler (daily/weekly/every N minutes/one time) and the usage gate (5 h / 7 d Claude limits) | Port of Burrow features |
| 3 | Token budgets (per agent, per company) and an Overview page with per-agent/per-person usage | Port of Burrow features |
| 4 | Turkish / English interface (vault setting) | Burrow has it |
| 5 | Retry of failed runs, chat export, notifications (Telegram) | Port of Burrow features |
| 6 | Installer, app icon, light theme | Polish |
| 7 | A fixed tunnel address (named Cloudflare tunnel with the owner's account) | The quick tunnel address changes at every start |

### About item 1
Today a company uses the host's providers only. Design sketch: each Phantom Co. can be a *worker*: the host adds a worker by invitation (mutual key pinning); the agent's `provider` can be `codex@friend`; the host sends `{job, prompt, session}` and receives the event stream; files are exchanged through the project folder (the worker mirrors it, or the job uses the host's folder over a share). Needs a protocol (`Project/Protocol`) and a sync or share story for project folders.

## Open questions
- Should friends' own companies be listed (switch from the sidebar) instead of the "home" button?
- Where should large files live (UE assets, video) once projects are shared between PCs?
