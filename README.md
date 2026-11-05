# 👻 Phantom Co.

**A ghost company on your PC.** Hire AI agents ("ghosts") with roles, skills and a look, chat with them, give them tasks and projects, and let them hand work to each other. Several people can share one company. The brains are the **Claude Code**, **Codex** and **Gemini CLI** command-line tools you already have on your PC.

Windows only. One program file, no installer, no cloud service of its own.

![A planner ghost hands work to a writer and a reviewer, then reports the result](Media/phantom_co_demo.gif)

## What it does

- **Ghost agents**: role, boss, persona, skills, brain (Claude Code, Codex, Gemini CLI or the free built-in Demo), model and time limit. 23 ghost forms and colours.
- **Several accounts per brain** (Settings > Secrets): besides the sign-in the CLI already has, add more subscription accounts or API keys (stored encrypted for your Windows user) and pick one per ghost. Gemini CLI works with an API key only.
- **Chats** (private or shared) and **agent-to-agent hand-over**: a ghost can ask a colleague, and the answer comes back to it automatically.
- **Projects**: a work folder plus notes and per-ghost jobs that every ghost working there reads.
- **Tasks**: a job for a ghost with its own conversation and run history. Tasks can wake up on a schedule (every N minutes or daily) and can start from templates.
- **Skills** are the only source of tools: a ghost can only do what its skills allow, and only with the rights of the person who asked.
- **MCP servers** (Settings > MCP) give ghosts outside tools: a real browser (Playwright), uploading to your YouTube channel, a Telegram bot, or MCP servers you (or a ghost with the MCP builder skill) write yourself. Secrets stay encrypted on your PC, and a call can wait for your approval in the chat before it acts.
- **Several people**: owner / editor / viewer roles, invitation links, private chats. Friends connect over the local network or through an optional Cloudflare quick tunnel. A friend can also lend their own Claude/Codex to the company (experimental: deletions are not sent back, big folders are slow, both PCs need the same version).
- **Runs** with a live view, stop button, tokens and cost; a failed run can be tried again with one click. A **Files** browser. English and Turkish interface, light and dark colour themes. Works on a phone browser too.
- Everything is stored as plain Markdown/JSON files in a folder (the **Vault**, `Documents\Phantom_Vault`).

## Get started

1. Download `phantom_co.exe` from the [latest release](https://github.com/mthnyuksek/Phantom_Co/releases/latest) (or build it, see below).
2. Optional: install and sign in to [Claude Code](https://docs.anthropic.com/en/docs/claude-code) (`claude`), [Codex](https://github.com/openai/codex) (`codex`) and/or [Gemini CLI](https://github.com/google-gemini/gemini-cli) (`gemini`, needs a Gemini API key from Google AI Studio, added in Settings > Secrets). Without them only the Demo brain works.
3. Double-click `phantom_co.exe`. The file is not code-signed, so Windows SmartScreen may say "Windows protected your PC": click **More info**, then **Run anyway**. On the first start it asks you to create the owner account and where to keep the Vault.
4. Hire the **guide ghost** when it is offered (or skip it). It knows the whole program, answers your questions and helps you plan your first team.

**Updating**: Settings > About shows your version and, twice a day, asks GitHub whether a newer release exists (nothing is downloaded or installed by itself; switch it off there if you like). To update, quit Phantom Co. from its tray icon and replace `phantom_co.exe` with the new file. The Vault and your settings stay as they are. Friends who lend their AI should use the same version as the company.

![First start: create the company, hire the guide and ask it which ghosts to hire](Media/phantom_co_first_start.gif)

Requires the Microsoft Edge WebView2 Runtime, which ships with Windows 11 and current Windows 10.

## Build from source

Requirements: MinGW-w64 GCC 13+, CMake 3.20+, Ninja. Python 3 for the tests.

```
cmake -S Project\Node_Daemon -B Project\Node_Daemon\Build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Project\Node_Daemon\Build
python -X utf8 -I Project\Node_Daemon\Tests\smoke_test.py <full path of phantom_co.exe>
```

The path of the project must not contain spaces. Command-line options: `phantom_co.exe --help`.

## Before you share your company

- **Sharing and the tunnel use plain HTTP** inside the program (the tunnel adds HTTPS on the internet side). Turn network sharing on only in a private network you trust. Every connection still needs a sign-in.
- **Agents act on your PC** with the tools their skills give them (file writing, and for the owner shell commands). Give powerful skills only to ghosts you trust and check what they do in the Runs tab.
- **MCP servers are programs that run on your PC** with the secrets you give them. Add only servers you trust, and keep "Ask me before it acts" on for anything that sends, posts or deletes.
- **Subscription terms**: Phantom Co. never shares a login, and work always runs on the PC where the CLI is signed in. Still, letting other people use your Claude, Codex or Gemini access may be limited by Anthropic's, OpenAI's or Google's terms. Check them yourself before you rely on it.

## License

[MIT](LICENSE) © 2026 Metehan Yüksek. Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Phantom Co. is an independent project and is not affiliated with Anthropic, OpenAI, Google, Microsoft or Cloudflare.
