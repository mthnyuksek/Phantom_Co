# Third-party notices

Phantom Co. includes the following code written by others. Their licenses apply to those parts.

| Component | Version | Where | License |
| --- | --- | --- | --- |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) by Yuji Hirose | 0.26.0 | `Project/Node_Daemon/Third_Party/httplib.h` | MIT (header of the file) |
| [JSON for Modern C++](https://github.com/nlohmann/json) by Niels Lohmann | 3.11.3 | `Project/Node_Daemon/Third_Party/json.hpp` | MIT (header of the file) |
| [Microsoft Edge WebView2 SDK](https://developer.microsoft.com/microsoft-edge/webview2/) (headers and `WebView2Loader.dll`) | | `Project/Node_Daemon/Third_Party/Web_View2/` | BSD-style, see `Project/Node_Daemon/Third_Party/Web_View2/LICENSE.txt` |

The built `phantom_co.exe` contains all three.

Phantom Co. starts, but does not include, these programs when they are installed on the PC: the [Claude Code](https://docs.anthropic.com/en/docs/claude-code) CLI, the [Codex](https://github.com/openai/codex) CLI and, for internet access, [cloudflared](https://github.com/cloudflare/cloudflared) (downloaded on demand from Cloudflare's GitHub releases). Each is covered by its own license and terms.
