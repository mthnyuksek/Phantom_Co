# Build and run (Windows)

## Requirements
- MinGW-w64 GCC 13+ (tested with GCC 15.2), CMake 3.20+, Ninja.
- Microsoft Edge WebView2 Runtime (ships with Windows 11 and current Windows 10).
- Optional: Claude Code CLI (`claude`) and/or Codex CLI (`codex`), signed in on this PC. Without them only the built-in **Demo** brain works (simulated, free).
- Python 3 for the smoke test.

## Build
```
cmake -S Project\Node_Daemon -B Project\Node_Daemon\Build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Project\Node_Daemon\Build
```
Result: `Project\Node_Daemon\Build\phantom_co.exe`, one static file (the web pages, templates and the WebView2 loader are inside). A running `phantom_co.exe` locks the file: close it before building.

## Run
Double-click `phantom_co.exe` (or run it from a terminal).
- First start: the program asks you to create the owner account and where to keep the Vault (default `<Documents>\Phantom_Vault`; "Browse…" picks another folder).
- Settings are in `%LOCALAPPDATA%\Phantom_Co\node.json`; accounts and sessions in `%LOCALAPPDATA%\Phantom_Co\auth.json`.
- Closing the window quits the program (running agents are stopped). A second start only brings the first window to the front.

| Option | Meaning |
| --- | --- |
| `--vault <folder>` | Use another Vault for this run |
| `--port <n>` | Another port for the local server (default 7777) |
| `--share` | Let people on the network connect (the owner can also switch it in Settings) |
| `--data <folder>` | Folder for accounts and settings instead of `%LOCALAPPDATA%\Phantom_Co` (a second company, tests) |
| `--no-window` | Server only, no window (tests; output goes to the terminal) |
| `--devtools` | Keep browser shortcuts and F12 in the window |
| `--ui-dir <folder>` | Read `Project\Web_Ui` from disk: edit the UI and press F5 (with `--devtools`), no rebuild |

## Test
```
python -X utf8 -I Project\Node_Daemon\Tests\smoke_test.py Project\Node_Daemon\Build\phantom_co.exe
python -X utf8 -I Project\Node_Daemon\Tests\smoke_test.py <exe> --real claude    # a little subscription quota
python -X utf8 -I Project\Node_Daemon\Tests\smoke_test.py <exe> --real codex
```
About 120 checks: Vault creation, accounts and roles, invitations, chats (private/shared), agents handing work to each other, stop/failure, skills and role rights, projects, tasks, files and path escapes, sharing switch, restart, upgrade of a version 1 Vault. It runs the program with `--no-window` on a temporary Vault and data folder, port 7791.

## Testing the real window
Start with the environment variable `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223 --remote-allow-origins=*`, then talk to `http://127.0.0.1:9223/json` with a DevTools-protocol client (click, type, screenshot). Node 24 has a global `WebSocket`.

## Using it
1. **The Haunt**: hire ghosts (templates: Planner, Developer, Writer, Reviewer, Researcher, Fixer), pick form and colour, skills, boss, brain. "✨" cycles the form.
2. **Chats**: talk to a ghost; ghosts can ask each other (`<send>`), you see it in the chat and in the Agents tab.
3. **Projects**: a folder + notes every ghost working in it reads.
4. **Tasks**: give a ghost a job; the conversation is the task page; "Run again" with a note.
5. **Team** (owner): add people, send invitation links. **Settings** (owner): sharing, limits, Vault folder.
