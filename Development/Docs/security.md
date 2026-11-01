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

## Lending a friend's AI (provider workers)
- The friend's program authenticates with its own worker token (Bearer, one per user, only a SHA-256 stored, ended by a new token, a password change, or removing the user). The worker API (`/api/worker/*`) accepts only that token and only editors/owners; file calls work only for the folder of a run that this worker currently holds.
- The host decides the prompt, so lending is an act of trust by the friend: it is off until the friend switches it on in the profile, and says so. The friend's program starts only `claude` / `codex`, only with file and web tools (Bash/PowerShell are removed by the host and again by the worker), without extra folders, inside a mirror folder under `%LOCALAPPDATA%\PC2\Worker`. Model and session strings from the host are checked against a whitelist because they reach a command line (`.cmd` files run through `cmd.exe`). Residual risk: a malicious host could still try to make Codex's read-only sandbox read other files of the friend's PC, because Codex can read outside the working folder.
- The new-account link on the sign-in screen (`/api/auth/register`) only works from the PC that runs the program (loopback address **and** a local Host header **and** no Cloudflare/forwarding header, because tunnel traffic also arrives from 127.0.0.1), never over the network or the tunnel.
- Internet access (Cloudflare quick tunnel): off at every start, the owner switches it on in Settings. The address is random and changes each time, traffic is encrypted (https), sign-in is still required for everything. While it is on, anybody who knows the address can see the sign-in page and try passwords (throttled per person and address, 5 tries then waiting). Switch it off when it is not needed. The local server keeps listening on 127.0.0.1 only; the tunnel program (`cloudflared`, downloaded from Cloudflare's GitHub release) is the only thing that reaches it.
- File exchange: uploads need editor, deletes owner; 200 MB per upload; same path rules as the file API.

## Threats considered
| Threat | Mitigation |
| --- | --- |
| Other web site calls the local API | Host check, `X-PC2`, Origin check, session cookie SameSite=Strict |
| Someone on the network guesses passwords | Throttling, long hashes, sharing is off by default |
| Editor makes an agent run commands or promote themselves | `min_role` per skill, owner-only skills/rules/users, role data outside the Vault |
| Prompt injection from a file or chat | Tools only through skills, no MCP, deny-by-default permissions, owner-only powerful skills |
| Strangers find the tunnel address | Random address, off by default, sign-in + throttling, owner can switch off at once |
| Tunnel traffic mistaken for "this PC" | `is_this_pc` checks Host and forwarding headers, not only the address |
| Host abuses a friend's lent AI | Friend switches it on knowingly, file/web tools only, mirror folder, validated command-line strings |
| One person drains the subscription | Per-person queue limit, concurrency limit, time and turn limits |
| Private chat read by others | Visibility check in every chat/run/job endpoint (the owner can read all) |
| Traffic sniffed on the network | Not handled: use a private network (Tailscale) |
