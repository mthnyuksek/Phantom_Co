# 0003 – v1 is local only; the program creates the Vault

- **Status**: Accepted (2026-11-01)
- **Context**: Networking, pairing and sync are the riskiest parts. The core
  product (company, roles, tasks, agent runs) is useful on one PC.
- **Decision**: v1 runs on a single PC, listens on `127.0.0.1` only, and has no
  peer link or sync. The Vault is created by the program on first run from an
  embedded template instead of being set up by hand. Two-PC features are v2.
- **Consequences**: Faster first release. v1 must still use ULIDs, file-based
  state, provider adapters and a versioned message protocol so v2 is additive.
  Amends decisions 0001 and 0002 (their timing, not their direction).
