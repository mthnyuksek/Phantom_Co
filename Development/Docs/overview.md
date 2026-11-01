# Phantom Co. – Overview

## Goal

A Paperclip-style "ghost company": agents with roles, skills and a ghost look, working for **several people** who use one shared company. Built on the same ideas as the owner's Burrow project (see `../project_summary.md` and `multi_user.md`), with **Claude** (Claude Code CLI) and **Codex** (Codex CLI) as brains.

- A normal Windows program, `phantom_co.exe`, with its own window (WebView2). Not a web site.
- One PC (the host) holds the company. Friends use their own Phantom Co. program and connect to the host.
- Nobody's subscription login is ever shared: work runs where the CLI is signed in.

## What exists

- **Ghost agents**: 11 forms, colors, moods, shapeshifting; role, boss (`reports_to`), persona, skills, brain (Claude/Codex/Demo), model, time limit.
- **Skills** (the only source of tools), **rules**, **projects** (work folder + shared notes), **tasks** (each with its conversation), **chats** (private or shared) and **agent-to-agent hand-over** with reports back.
- **Users**: owner / editor / viewer, invitations, private chats, agents acting with the asker's rights.
- **Runs** with live view, stop, tokens and cost; **files** browser; **Vault** of plain Markdown files, created by the program.

## Vocabulary

| Term | Meaning |
| --- | --- |
| **Host** | The PC that runs the company (Vault, CLIs). |
| **Vault** | The folder of plain files (`<Documents>\PC2_Vault`). See `vault_format.md`. |
| **Agent / ghost** | A worker with role, persona, skills and a brain. |
| **Skill** | Tools + instructions an agent may use. Agents get tools only through skills. |
| **Project** | A folder where agents work, plus notes every agent working there reads. |
| **Task** | A job for an agent; it has its own conversation and run history. |
| **Run** | One execution of a CLI for a job (record + event log). |
| **Reply stack** | Who is waiting for an answer: user -> A -> B. B's answer goes back to A, A's to the user. |
| **Brain / provider** | `claude`, `codex`, or the built-in `demo`. |

## Tech

C++20, CMake/Ninja/MinGW, one static executable. Win32 window + WebView2; cpp-httplib server on `127.0.0.1`; nlohmann/json; BCrypt (Windows) for hashing. UI: plain ES modules, no build step, dark theme. See `../project_summary.md`.

## Non-goals (for now)

- Pooling the Claude of one PC and the Codex of another inside one company (needs provider workers, see `roadmap.md`).
- Internet exposure or encryption on the wire (use a private network).
- macOS / Linux.

## Docs index

- `running.md` – build, run, test
- `architecture.md` – processes, threads, data flow
- `vault_format.md` – every file in the Vault
- `multi_user.md` – accounts, roles, privacy, sharing
- `agent_runtime.md` – how Claude/Codex are launched
- `security.md` – threats and protections
- `roadmap.md` – status and next steps
- `../Decisions/` – decision records
