# 0004 – A normal Windows program with its own window

- **Status**: Accepted (2026-11-01)
- **Context**: The first working version opened the default browser. The owner wants a normal `.exe` (like Burrow), not a web site.
- **Decision**: `phantom_co.exe` is a Win32 GUI program that hosts the web interface in WebView2 (reusing Burrow's wrapper). The web server stays, bound to `127.0.0.1`, because it is also the way other people connect when sharing is switched on. The window navigates to the local address; a second start of the program only focuses the first window. A page can ask the program (`postMessage`) to open a friend's company or return home. Other sites never replace the app page (they open in the normal browser).
- **Alternatives rejected**: WebView2 only with a private bridge like Burrow (no way for friends to connect without a second transport); a browser tab (not wanted).
- **Consequences**: One process, one file (UI, templates and loader DLL are embedded). `--no-window` keeps the server testable. The WebView2 runtime must exist (it ships with Windows 11 and current Windows 10).
