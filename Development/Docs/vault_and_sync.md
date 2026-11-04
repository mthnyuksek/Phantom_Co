# Vault creation and (future) sync

## Creation (done)
The program creates the Vault itself, nothing is set up by hand. `init_vault` (in `store.cpp`):
1. If the folder does not exist or is empty: create it, write `vault.json` (version 2, new `vault_id`), the folders, the starting files from the embedded `Vault_Template`, and the default skills from the embedded `Skill_Templates`.
2. If `vault.json` exists: check the version; a version 1 Vault is upgraded (see `vault_format.md`); newer unknown versions are refused with a clear message.
3. A non-empty folder that is not a Vault is refused. Existing files are never overwritten.
Default place: `<Documents>\Phantom_Vault` (visible on purpose); change it in Settings (owner) or with `--vault`. Changing the folder re-points the program; old files are not moved.

## Sync (not built; only needed for provider workers)
Friends do not need a synced Vault: they connect to the host's program. Syncing only matters when two PCs must work on the same project folder (provider workers, see `roadmap.md`). If it comes:
- Start with Syncthing on `Projects/` and `Shared_Files/` only (never `Conversations/`, `Runs/`, `Users/`), with the usual conflict copies.
- Never sync `%LOCALAPPDATA%\Phantom_Co` (accounts, sessions, node config).
- Later maybe a built-in sync over the worker link (index of path/size/mtime/hash, chunked transfer, atomic rename, tombstones).
