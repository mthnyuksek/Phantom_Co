# Multi-user design

## The picture
One PC runs the company (the **host**): it has the Vault, the program, and the Claude / Codex command line tools signed in with the host owner's subscription. Other people use **their own Phantom Co. program** (a normal window) and connect to the host. Nobody's login ever leaves the PC it was created on; a friend's request is run by the host's CLI, on the host's PC, while the host is on.

```
 friend's phantom_co.exe  --(http, private network)-->  host's phantom_co.exe  --> claude / codex CLI
   window (WebView2)                               server + Vault
```
The friend's window just shows the host's web page (`window.chrome.webview.postMessage({cmd:'connect', url})` navigates, `{cmd:'home'}` comes back to the friend's own company). A friend can also run their own company at the same time (same program, own Vault, own port).

## Accounts
- First start: "Create company" makes the **owner**. Everyone else is invited by the owner (Team page): the owner picks a name and a role, the program makes a one-time link `http://<address>:<port>/#/join/<token>` (valid 3 days, one use). The friend pastes it into "I was invited to a friend's company" or opens it, and chooses their own password.
- Password: PBKDF2-SHA256, 200,000 iterations, random salt, constant-time compare, minimum 8 characters. Failed logins are throttled per name and address (5 failures -> wait, growing).
- Session: random 192-bit token, only its SHA-256 is stored, HttpOnly + SameSite=Strict cookie `pc2_session_<port>`, 30 days. Changing a password ends the old sessions.
- Role and password data live in `%LOCALAPPDATA%\PC2\auth.json`, outside the Vault: an agent that may edit Vault files cannot read hashes or promote anybody.

## Roles
| | viewer | editor | owner |
| --- | --- | --- | --- |
| read agents, skills, rules, projects, shared chats, tasks, runs, files | yes | yes | yes |
| chat, create tasks and projects, hire/edit ghosts, notes | no | yes | yes |
| skills, rules, company, settings, team, Vault folder, sharing | no | no | yes |
| see every chat | no | no | yes |

## Privacy
A chat belongs to the person who started it and is hidden from others unless they tick "Share with team". Task conversations are team-wide. Agent-to-agent conversations inherit the visibility of the chat that started them. Run records and live jobs follow the same rule. The owner can read everything, and everything is a plain file in the Vault.

## Agents act with the rights of the asking person
A skill has `min_role`. When Ece (editor) chats with an agent, skills with `min_role: owner` are left out of that run; when the owner asks the same agent, they are included. So an editor cannot make an agent run commands or edit agent/skill/rule files. Skills with Bash/PowerShell or Vault folders other than Projects are forced to `owner`; only the owner may create or edit skills.

## Fair use of one subscription
`max_concurrent` runs per brain (default 1), an agent never runs twice at once, `max_queue_per_user` waiting runs per person (default 10), a time limit per run, `max_turns` per run, a hop limit for agent-to-agent chains.

## Network
**Internet access** (Settings → Internet access, owner): a free Cloudflare quick tunnel gives the company an encrypted `https://….trycloudflare.com` address that works from anywhere; invitations then carry that address. Off at every start.
Off by default: the server listens on `127.0.0.1` and only accepts local Host headers. "Let people on my network connect" (Settings, owner) makes it listen on all addresses and restarts the listener. Windows asks once about the firewall (allow private networks). The connection is plain HTTP: use a home network or a private overlay (Tailscale), never the open internet. Sign-in is required for everything except the page itself and the sign-in calls.

## Limits of this design (today)
- An agent uses the host's providers, or - with `provider: claude@<friend>` / `codex@<friend>` - the CLI of a friend whose program lends it (see `Decisions/0006_provider_workers.md`). The friend's PC must be on, the program open and "Lend my AI" switched on; otherwise the run fails after 90 s.
- The host PC must be on and Phantom Co. open.
- No encryption on the wire (see Network).
