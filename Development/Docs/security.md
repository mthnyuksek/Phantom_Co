# Security

## Accounts and subscriptions
- A subscription login stays on the PC where the CLI is signed in. The program never copies, syncs or sends tokens, cookies or API keys. Friends do not log in as the host: they send requests that the host's CLI runs on the host's PC.
- Re-check the Claude and Codex subscription terms for this kind of shared use before relying on it; terms change. The design keeps the login on its own machine, which is the safest reading, but it is not a guarantee.

## The local server
- Default: listens on `127.0.0.1`; the `Host` header must be local (stops DNS rebinding).
- Every non-GET request needs `X-PC2: 1` and a same-origin `Origin` (stops other web sites posting to it). Checked after the body is read (`set_pre_request_handler`).
- Every API route except `auth/status|setup|login|logout|invite|join` and the static page needs a session and a minimum role.
- Sharing on: listens on all addresses, any Host accepted, sign-in still required. Plain HTTP: use a private network.

## Accounts
PBKDF2-SHA256 (200k iterations, random salt), constant-time compare, login throttling, random 192-bit session/invite tokens stored only as SHA-256, HttpOnly + SameSite=Strict cookie named per port, one-time invitations that expire after 3 days, minimum password length 8. `setup` works only while no user exists. At least one owner always remains.

## Agents and files
- Agents get tools only through skills, and only the skills the asking person's role may use. Commands (Bash/PowerShell) and Vault folders other than Projects are owner-only.
- Claude's Bash/PowerShell tools are **not** path-sandboxed: an owner who grants a command skill gives that agent the owner's rights on the PC. Codex runs in its own sandbox.
- Users' password hashes, roles and sessions are outside the Vault; conversations and runs are inside it but never added to an agent's accessible folders. (An agent with `Read` and a Vault-wide skill could still read what is in its working folders.)
- File API: paths are resolved inside the chosen folder (`Shared_Files` or a project); `..`, absolute paths and the hidden `.Pc2` metadata are refused; images are served with `Content-Security-Policy: sandbox`.
- User-supplied model names are checked against a character whitelist (they reach a command line).

## Threats considered
| Threat | Mitigation |
| --- | --- |
| Other web site calls the local API | Host check, `X-PC2`, Origin check, session cookie SameSite=Strict |
| Someone on the network guesses passwords | Throttling, long hashes, sharing is off by default |
| Editor makes an agent run commands or promote themselves | `min_role` per skill, owner-only skills/rules/users, role data outside the Vault |
| Prompt injection from a file or chat | Tools only through skills, no MCP, deny-by-default permissions, owner-only powerful skills |
| One person drains the subscription | Per-person queue limit, concurrency limit, time and turn limits |
| Private chat read by others | Visibility check in every chat/run/job endpoint (the owner can read all) |
| Traffic sniffed on the network | Not handled: use a private network (Tailscale) |
