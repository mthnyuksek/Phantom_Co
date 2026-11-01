# 0001 – Peer architecture, tasks travel, credentials don't

- **Status**: Accepted (2026-11-01)
- **Context**: Two PCs, one with a Claude subscription, one with Codex. Each
  owner wants to use both agents.
- **Decision**: Two equal C++ Nodes (no central server). Tasks and files travel
  between Nodes; each provider CLI runs only on the PC where it is logged in.
  Shared state is files in a synced Vault.
- **Alternatives rejected**: sharing logins/tokens (terms + security risk);
  cloud backend (cost, privacy, unnecessary); remote-desktop only (no
  orchestration).
- **Consequences**: A task for a provider needs that PC to be on; queueing is
  required. File sync becomes a core subsystem.
