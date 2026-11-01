# 0002 – Use Syncthing first, build own sync later

- **Status**: Accepted (2026-11-01)
- **Context**: Vault sync is hard to get right (conflicts, partial writes,
  deletes). It is not the product's differentiator.
- **Decision**: M0–M5 use Syncthing as transport. Replace with built-in
  `vault_sync` at M6 once the rest works.
- **Consequences**: One extra install per PC for now; the Node must read
  Syncthing conflict files. Vault layout must stay transport-agnostic.
