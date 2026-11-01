# 0005 – Burrow's model, for many users

- **Status**: Accepted (2026-11-01). Amends 0003 (v1 local only): friends can now connect to a host.
- **Context**: The owner's Burrow project has the system he wants here: Markdown vault, skills as the only source of tools, projects with notes, agents with bosses, chats and agent-to-agent hand-over with a reply stack, lean prompts. PC2 must work for several people.
- **Decision**: Port the concepts (not the code: Burrow is Claude-only, single-user, native bridge). Add: accounts with roles, invitations, privacy of chats, agents acting with the asking person's rights, per-person queue limits, Codex as a second brain with session resume, network sharing. Secrets (hashes, sessions) stay outside the Vault.
- **Not ported yet**: wake-up scheduler, usage gate and token budgets, overview statistics, Telegram, Cloudflare tunnel, Turkish/English UI, retry of failed runs, chat export.
- **Consequences**: Vault format 2 (with automatic upgrade of version 1). The earlier idea of syncing two Vaults (0002) is not needed for friends to join; it only matters for pooling providers of two PCs.
