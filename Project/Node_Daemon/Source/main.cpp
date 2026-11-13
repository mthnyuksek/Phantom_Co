#include <winsock2.h>  // before windows.h (free port check for a new Vault window)
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <set>
#include <thread>

#include "accounts.hpp"
#include "budget.hpp"
#include "notify.hpp"
#include "auth.hpp"
#include "mcp.hpp"
#include "mcp_builtin.hpp"
#include "telegram_bridge.hpp"
#include "runner.hpp"
#include "script_expr.hpp"
#include "script_runner.hpp"
#include "server.hpp"
#include "store.hpp"
#include "tunnel.hpp"
#include "version.hpp"
#include "web_view_host.hpp"
#include "worker.hpp"

using namespace pc2;

// One copy of the program = one "instance" = one data folder (node.json, auth.json, webview_data) = one Vault, one port.
// The default instance uses %LOCALAPPDATA%\Phantom_Co, more Vault windows use %LOCALAPPDATA%\Phantom_Co\Instances\<n>
// (started from the tray menu with --data). Closing the window only hides it: the company keeps working in the hidden
// icons until "Quit" in the tray menu.
namespace {
constexpr wchar_t kWindowClass[] = L"PhantomCoWindow";
constexpr wchar_t kInstanceProp[] = L"PhantomCo.Instance";  // window property = instance key, so a second start finds its own window
constexpr UINT kServerFailed = WM_APP + 1;
constexpr UINT kPickFolder = WM_APP + 2;  // the page asked for a folder picker (shown from the window loop, not inside a WebView2 callback)
constexpr UINT kTrayMsg = WM_APP + 3;
constexpr UINT kShowMsg = WM_APP + 4;     // another start of the same instance asks this window to come back
constexpr UINT_PTR kTitleTimer = 1;
enum : UINT { kMenuShow = 1, kMenuNew = 2, kMenuQuit = 3, kMenuInstance = 100 };

WebViewHost* g_host = nullptr;
HWND g_hwnd = nullptr;
WorkerClient g_worker;
Config* g_cfg = nullptr;
Store* g_store = nullptr;
Runner* g_runner = nullptr;
ScriptRunner* g_scripts = nullptr;
std::wstring g_home_url;
fs::path g_data_dir;
std::wstring g_extra_args;  // --devtools / --ui-dir, passed on to Vault windows started from the tray
bool g_quitting = false, g_told_hidden = false;
bool g_tr = false;          // Turkish tray texts (the page tells us its language)
std::wstring g_title;
UINT g_taskbar_created = 0;
std::vector<fs::path> g_menu_instances;

const wchar_t* T(const wchar_t* en, const wchar_t* tr) { return g_tr ? tr : en; }

BOOL WINAPI on_ctrl(DWORD) {
    stop_server();
    return TRUE;
}

void fatal(const std::wstring& msg, bool window_mode) {
    if (window_mode) MessageBoxW(nullptr, msg.c_str(), L"Phantom Co.", MB_OK | MB_ICONERROR);
    else std::wcerr << msg << L"\n";
}

// --builtin-call (Tests/reddit_test.py, reminders_test.py): one request on stdin {builtin, tool, args, secrets, options, telegram_ids}, the result on stdout.
// An empty tool = the check of Settings > MCP. Only does what the built-in server does for a ghost, with what it is given.
int builtin_call_main() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE) return 2;
    std::string text;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(in, chunk, sizeof chunk, &got, nullptr) && got > 0) text.append(chunk, got);
    const json r = json::parse(text, nullptr, false);
    if (!r.is_object()) return 2;
    const json secrets = r.value("secrets", json::object()), options = r.value("options", json::object());
    const std::string builtin_name = r.value("builtin", ""), tool = r.value("tool", "");
    const json session = {{"secrets", secrets}, {"options", options}, {"workdir", ""}, {"telegram_ids", r.value("telegram_ids", json::array())}};
    const std::string a = (tool.empty() ? builtin::check(builtin_name, secrets, options) : builtin::call(builtin_name, tool, r.value("args", json::object()), session)).dump() + "\n";
    DWORD n = 0;
    WriteFile(out, a.data(), (DWORD)a.size(), &n, nullptr);
    return 0;
}

// --script-eval: reads request lines from the stdin pipe, writes one answer line each (no console, like the MCP proxy).
int script_eval_main() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE) return 2;
    std::string buf;
    char chunk[4096];
    DWORD got = 0;
    auto answer = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        std::string a = script::script_eval_line(line) + "\n";
        DWORD n = 0;
        WriteFile(out, a.data(), (DWORD)a.size(), &n, nullptr);
    };
    while (ReadFile(in, chunk, sizeof chunk, &got, nullptr) && got > 0) {
        buf.append(chunk, got);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            answer(buf.substr(0, nl));
            buf.erase(0, nl + 1);
        }
    }
    answer(buf);
    return 0;
}

void usage() {
    std::cout << "Phantom Co. " PC2_VERSION_STR " - ghost company\n"
                 "  --vault <folder>   Vault folder for this run (default: saved setting, then Documents\\Phantom_Vault)\n"
                 "  --port <number>    Port of the local server (default 7777)\n"
                 "  --share            Let people on the network connect (the owner can also switch it in Settings)\n"
                 "  --data <folder>    Folder for accounts and settings (default %LOCALAPPDATA%\\Phantom_Co).\n"
                 "                     Every data folder is its own copy of the program: its own Vault, port and window\n"
                 "  --no-window        Run only the server, without a window (tests, a PC without a screen)\n"
                 "  --worker-url <address> --worker-token <token>\n"
                 "                     Lend this PC's Claude/Codex to the company at that address (normally switched on in the window)\n"
                 "  --devtools         Keep browser shortcuts and F12 in the window (development)\n"
                 "  --ui-dir <folder>  Read the web pages from disk instead of the built-in copy (development)\n"
                 "  --version          Print the version and quit\n";
}

// ------------------------------------------------------------------ instances
std::wstring norm(const fs::path& p) {
    std::wstring s = p.lexically_normal().make_preferred().wstring();
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    for (auto& ch : s) ch = towlower(ch);
    return s;
}
uintptr_t instance_key(const fs::path& data_dir) {
    uint32_t h = 2166136261u;  // FNV-1a: stable across runs (std::hash is not promised to be)
    for (wchar_t ch : norm(data_dir)) { h ^= static_cast<uint32_t>(ch); h *= 16777619u; }
    return h | 1u;  // never 0 (GetProp answers 0 for "no property")
}
std::wstring mutex_name(const fs::path& data_dir) { return L"Local\\PhantomCo_Instance_" + std::to_wstring(instance_key(data_dir)); }
bool instance_running(const fs::path& data_dir) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, mutex_name(data_dir).c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}
HWND find_instance_window(const fs::path& data_dir) {
    struct Find { uintptr_t key; HWND found; } f{instance_key(data_dir), nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* f = reinterpret_cast<Find*>(lp);
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 64);
        if (wcscmp(cls, kWindowClass) == 0 && reinterpret_cast<uintptr_t>(GetPropW(w, kInstanceProp)) == f->key) { f->found = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}
void wake_window(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(w, kShowMsg, 0, 0);
}

fs::path instances_dir() { return config_dir() / L"Instances"; }
// The default data folder first, then Instances\2, \3, ... (only folders that hold a node.json).
std::vector<fs::path> known_instances() {
    std::vector<fs::path> out{config_dir()};
    std::vector<std::pair<int, fs::path>> more;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(instances_dir(), ec)) {
        int n = _wtoi(e.path().filename().c_str());
        if (n > 0 && fs::exists(e.path() / L"node.json", ec)) more.push_back({n, e.path()});
    }
    std::sort(more.begin(), more.end());
    for (auto& m : more) out.push_back(m.second);
    return out;
}
std::wstring instance_label(const fs::path& data_dir) {
    Config c = load_config(data_dir / L"node.json");
    std::error_code ec;
    json co = read_json(c.vault / L"Company" / L"company.json");
    std::wstring folder = c.vault.filename().wstring();
    if (!fs::exists(c.vault / L"vault.json", ec)) return folder + T(L"  (not set up yet)", L"  (henüz kurulmadı)");
    std::wstring name = Widen(co.value("name", ""));
    return name.empty() || name == folder ? folder : name + L"  —  " + folder;
}
bool port_free(int port) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return true;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bool ok = s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    return ok;
}
void launch_instance(const fs::path& data_dir) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --data \"" + data_dir.wstring() + L"\"" + g_extra_args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
// A new Vault window: Instances\<n> with its own node.json (a free port, a new Vault folder next to the default one,
// changed on its setup screen like on a first start).
void new_instance() {
    std::set<int> ports;
    for (auto& d : known_instances()) ports.insert(load_config(d / L"node.json").port);
    int n = 2;
    std::error_code ec;
    while (fs::exists(instances_dir() / std::to_wstring(n), ec)) ++n;
    fs::path dir = instances_dir() / std::to_wstring(n);
    fs::create_directories(dir, ec);
    Config c;
    c.file = dir / L"node.json";
    c.port = 7778;
    while (c.port < 65000 && (ports.count(c.port) || !port_free(c.port))) ++c.port;
    fs::path base = default_vault_path().parent_path();
    int v = n;
    while (fs::exists(base / (L"Phantom_Vault_" + std::to_wstring(v)), ec)) ++v;
    c.vault = base / (L"Phantom_Vault_" + std::to_wstring(v));
    save_config(c);
    launch_instance(dir);
}

// ------------------------------------------------------------------ tray icon
void tray(DWORD action) {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    if (action == NIM_DELETE) { Shell_NotifyIconW(NIM_DELETE, &nid); return; }
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcsncpy_s(nid.szTip, g_title.c_str(), _TRUNCATE);
    Shell_NotifyIconW(action, &nid);
    if (action == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}
void tray_balloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
// Title bar and tray tooltip name the company and the Vault folder, so several Vault windows can be told apart.
void refresh_title() {
    if (!g_hwnd || !g_store) return;
    std::wstring folder = g_store->root().filename().wstring(), name = Widen(g_store->company().value("name", ""));
    std::wstring t = L"Phantom Co. · " + (name.empty() || name == L"Phantom Co." ? folder : name + L"  —  " + folder);
    if (t == g_title) return;
    g_title = t;
    SetWindowTextW(g_hwnd, t.c_str());
    tray(NIM_MODIFY);
}
void show_window() {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (g_host) g_host->Focus();
}
void quit() {
    if (g_runner && g_runner->any_active()) {
        show_window();
        if (MessageBoxW(g_hwnd, T(L"Ghosts are still working. Quit anyway? Their work stops.", L"Hayaletler hâlâ çalışıyor. Yine de çıkılsın mı? Çalışmaları durur."),
                        L"Phantom Co.", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    } else if (g_scripts && g_scripts->any_active()) {  // e.g. a Delay block waiting: nothing runs, but the script is not done
        show_window();
        if (MessageBoxW(g_hwnd, T(L"A script is still running (it may be waiting in a Delay block). It goes on when Phantom Co. starts again. Quit anyway?",
                                  L"Bir betik hâlâ çalışıyor (bir Delay bloğunda bekliyor olabilir). Phantom Co. yeniden açılınca devam eder. Yine de çıkılsın mı?"),
                        L"Phantom Co.", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    g_quitting = true;
    DestroyWindow(g_hwnd);
}
void tray_menu(int x, int y) {
    HMENU menu = CreatePopupMenu(), vaults = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuShow, T(L"Open Phantom Co.", L"Phantom Co.'yu aç"));
    SetMenuDefaultItem(menu, kMenuShow, FALSE);
    g_menu_instances = known_instances();
    for (size_t i = 0; i < g_menu_instances.size() && i < 50; ++i) {
        const fs::path& d = g_menu_instances[i];
        bool self = norm(d) == norm(g_data_dir), running = self || instance_running(d);
        std::wstring label = instance_label(d) + (running && !self ? T(L"  (open)", L"  (açık)") : L"");
        AppendMenuW(vaults, MF_STRING | (self ? MF_CHECKED : 0), kMenuInstance + i, label.c_str());
    }
    AppendMenuW(vaults, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(vaults, MF_STRING, kMenuNew, T(L"New Vault window…", L"Yeni kasa penceresi…"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(vaults), T(L"Vaults", L"Kasalar"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, T(L"Quit Phantom Co.", L"Phantom Co.'dan çık"));
    SetForegroundWindow(g_hwnd);  // otherwise the menu does not close when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
    if (cmd == kMenuShow) show_window();
    else if (cmd == kMenuNew) new_instance();
    else if (cmd == kMenuQuit) quit();
    else if (cmd >= kMenuInstance && cmd - kMenuInstance < g_menu_instances.size()) {
        const fs::path& d = g_menu_instances[cmd - kMenuInstance];
        if (norm(d) == norm(g_data_dir)) show_window();
        else if (HWND w = find_instance_window(d)) wake_window(w);
        else if (!instance_running(d)) launch_instance(d);
    }
}

void report_worker() {
    if (!g_host) return;
    json i = g_worker.info();
    i["type"] = "worker_status";
    g_host->PostToPage(i.dump());
}

// A page asks the program to open another company (a friend's Phantom Co.) or to come back home.
void on_page_message(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object() || !g_host) return;
    std::string cmd = m.value("cmd", "");
    if (cmd == "lang") {
        g_tr = m.value("lang", "") == "tr";
    } else if (cmd == "worker_status") {
        report_worker();
    } else if (cmd == "worker_start") {
        std::string url = m.value("url", ""), token = m.value("token", ""), err;
        if (g_worker.start(url, token, err)) {
            if (g_cfg) { g_cfg->worker_url = url; g_cfg->worker_token = token; save_config(*g_cfg); }
        } else {
            json i = {{"type", "worker_status"}, {"running", false}, {"status", err}};
            g_host->PostToPage(i.dump());
            return;
        }
        report_worker();
    } else if (cmd == "worker_stop") {
        g_worker.stop();
        if (g_cfg) { g_cfg->worker_url.clear(); g_cfg->worker_token.clear(); save_config(*g_cfg); }
        report_worker();
    } else if (cmd == "pick_folder") {
        if (g_hwnd) PostMessageW(g_hwnd, kPickFolder, 0, 0);
    } else if (cmd == "home") {
        g_host->Navigate(g_home_url);
    } else if (cmd == "connect") {
        std::string url = m.value("url", "");
        bool ok = (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) && url.size() < 400 &&
                  url.find_first_of(" \t\r\n\"<>") == std::string::npos && url.find("//") + 2 < url.size();
        if (ok) g_host->Navigate(Widen(url));
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbar_created && g_taskbar_created) {  // Explorer restarted: the tray icon is gone, add it again
        tray(NIM_ADD);
        return 0;
    }
    switch (msg) {
        case WM_SIZE:
            if (g_host) g_host->Resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_SETFOCUS:
            if (g_host) g_host->Focus();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {900, 600};
            return 0;
        }
        case WM_TIMER:
            if (wp == kTitleTimer) refresh_title();
            return 0;
        case kShowMsg:
            show_window();
            return 0;
        case kTrayMsg:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                case WM_LBUTTONDBLCLK:
                    show_window();
                    break;
                case WM_CONTEXTMENU:
                    tray_menu(static_cast<short>(LOWORD(wp)), static_cast<short>(HIWORD(wp)));
                    break;
            }
            return 0;
        case kPickFolder: {
            json r = {{"type", "folder_picked"}, {"path", ""}};
            IFileOpenDialog* dlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
                DWORD o = 0;
                dlg->GetOptions(&o);
                dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dlg->SetTitle(L"Where should Phantom Co. keep its Vault?");
                IShellItem* item = nullptr;
                PWSTR picked = nullptr;
                if (SUCCEEDED(dlg->Show(hwnd)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &picked))) {
                    r["path"] = U8(vault_target(fs::path(picked)));  // a non-empty folder gets a Phantom_Vault folder inside
                    CoTaskMemFree(picked);
                }
                if (item) item->Release();
                dlg->Release();
            }
            if (g_host) g_host->PostToPage(r.dump());
            return 0;
        }
        case kServerFailed:
            MessageBoxW(hwnd, L"Phantom Co. could not start its local server. Another copy of Phantom Co. may already be running, or the port is in use.\n\nTry the option --port <number>.", L"Phantom Co.", MB_OK | MB_ICONERROR);
            g_quitting = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            // Windows wants to shut down / restart / sign out while the company is open: say no, so Windows shows
            // "Phantom Co. is preventing shutdown" with the reason below and a "Shut down anyway" button.
            // A critical shutdown (power failure, forced update) cannot be stopped.
            return (lp & ENDSESSION_CRITICAL) ? TRUE : FALSE;
        case WM_CLOSE:
            // The X button only hides the window: the company keeps serving ghosts and friends from the hidden icons.
            if (g_quitting) { DestroyWindow(hwnd); return 0; }
            ShowWindow(hwnd, SW_HIDE);
            if (!g_told_hidden) {
                g_told_hidden = true;
                tray_balloon(T(L"Phantom Co. is still running", L"Phantom Co. çalışmaya devam ediyor"),
                             T(L"Your ghosts keep working. Click the ghost in the hidden icons to open it again, right-click it to quit or to open another Vault.",
                               L"Hayaletleriniz çalışmaya devam ediyor. Tekrar açmak için gizli simgelerdeki hayalete tıklayın; çıkmak ya da başka bir kasa açmak için sağ tıklayın."));
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTitleTimer);
            tray(NIM_DELETE);
            RemovePropW(hwnd, kInstanceProp);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND make_window(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(12, 10, 20));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    int w = 1360, h = 860;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (g_data_dir.parent_path() == instances_dir()) {  // later Vault windows open a little lower, not exactly on top of the first
        int n = _wtoi(g_data_dir.filename().c_str());
        x += 32 * ((n - 1) % 6);
        y += 32 * ((n - 1) % 6);
    }
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Phantom Co. · Ghost Company", WS_OVERLAPPEDWINDOW, std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, inst, nullptr);
    SetPropW(hwnd, kInstanceProp, reinterpret_cast<HANDLE>(instance_key(g_data_dir)));
    // the right icon size for the title bar and for the taskbar / Alt+Tab
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    return hwnd;
}
}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    {   // An MCP server of a ghost's run (started by Claude/Codex/Gemini): stdin/stdout are its pipes, so no console and no window.
        int n = 0;
        wchar_t** av = CommandLineToArgvW(GetCommandLineW(), &n);
        if (av && n >= 2 && std::wstring(av[1]) == L"--mcp-proxy") {
            std::string name, key;
            int port = 0;
            for (int i = 1; i + 1 < n; ++i) {
                std::wstring a = av[i];
                if (a == L"--mcp-proxy") name = Narrow(av[++i]);
                else if (a == L"--mcp-port") port = _wtoi(av[++i]);
                else if (a == L"--mcp-key") key = Narrow(av[++i]);
            }
            return mcp_proxy_main(name, port, key);
        }
        // Test hook for script expressions (Tests/script_test.py): JSON lines on stdin -> answers on stdout (pipes).
        if (av && n >= 2 && std::wstring(av[1]) == L"--script-eval") return script_eval_main();
        if (av && n >= 2 && std::wstring(av[1]) == L"--builtin-call") return builtin_call_main();
    }
    // Started from a terminal? Then --help and errors can be printed there.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_tr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;  // until the page says which language it shows

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path vault_override, ui_dir, data_dir;
    bool window_mode = true, share = false, dev = false;
    int port_override = 0;
    std::string cli_worker_url, cli_worker_token;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--vault") vault_override = next();
        else if (a == L"--port") port_override = _wtoi(next().c_str());
        else if (a == L"--ui-dir") ui_dir = next();
        else if (a == L"--data") data_dir = next();
        else if (a == L"--no-window") window_mode = false;
        else if (a == L"--share") share = true;
        else if (a == L"--devtools") dev = true;
        else if (a == L"--worker-url") cli_worker_url = Narrow(next());
        else if (a == L"--worker-token") cli_worker_token = Narrow(next());
        else if (a == L"--version") { std::cout << "Phantom Co. " PC2_VERSION_STR "\n"; return 0; }
        else { usage(); return a == L"--help" ? 0 : 1; }
    }
    // the data folder decides which copy this is (its own node.json, accounts, Vault and port)
    g_data_dir = data_dir.empty() ? config_dir() : fs::absolute(data_dir);
    Config cfg = load_config(g_data_dir / L"node.json");
    if (port_override > 0) cfg.port = port_override;
    if (!vault_override.empty()) cfg.vault = fs::absolute(vault_override);
    if (share) cfg.share = true;
    if (dev) g_extra_args += L" --devtools";
    if (!ui_dir.empty()) g_extra_args += L" --ui-dir \"" + fs::absolute(ui_dir).wstring() + L"\"";

    // One copy per data folder: a second start just brings that window back (also from the hidden icons).
    SetLastError(0);  // earlier calls may have left ERROR_ALREADY_EXISTS behind
    HANDLE single = CreateMutexW(nullptr, TRUE, mutex_name(g_data_dir).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = find_instance_window(g_data_dir)) {
            wake_window(other);
            return 0;
        }
        fatal(L"Phantom Co. is already running with this data folder.", window_mode);
        return 3;
    }

    try {
        std::error_code vault_ec;
        bool vault_existed = fs::exists(cfg.vault, vault_ec);
        Store store(cfg.vault);
        Auth auth(cfg.file.parent_path() / L"auth.json");
        Accounts accounts(cfg.file.parent_path());  // extra Claude/Codex sign-ins of this copy (Settings > Secrets)
        Usage usage(cfg.file.parent_path(), &accounts);  // what is left of the Claude/Codex limits (script built-ins)
        Runner runner(store);
        runner.set_accounts(&accounts);
        runner.set_usage(&usage);
        Mcp mcp(cfg.file.parent_path(), store);  // MCP servers of this copy (Settings > MCP)
        mcp.set_port(cfg.port);
        runner.set_mcp(&mcp);
        // Visual scripts: prompt blocks are Runner jobs; the Runner tells the script runner when each job ends.
        ScriptRunner scripts(store, runner, [&auth, &store](const std::string& id) {
            auto u = store.user(id);
            return u && !u->value("disabled", false) && auth.exists(id) ? auth.role(id) : std::string();
        });
        // Notices: the bell, a Windows balloon, Telegram. Never deleted, like the Telegram bridge (a send may still be waiting at quit).
        auto* notifier = new Notifier(cfg.file.parent_path());
        runner.set_notifier(notifier);
        scripts.set_notifier(notifier);
        mcp.set_on_approval([&store, notifier](const std::string& user, const std::string& conv, const std::string& who, const std::string& tool) {
            auto cv = store.conv_load(conv);
            const std::string task = cv ? (*cv)["meta"].value("task_id", "") : "";
            notifier->post({"ask", user, who + " asks for your approval", tool, who + " onayını bekliyor", tool, task.empty() ? "#/chats/" + conv : "#/tasks/" + task, ""});
        });
        runner.set_finish_hook([&scripts, &store, &runner, notifier](const Job& j, const std::string& st, const std::string& a, const std::string& e) {
            scripts.on_job_finished(j, st, a, e);
            // Work a person asked for directly: a failed task always says so, anything else when it took long.
            if (j.stack.size() != 1 || j.stack[0].kind != "user" || is_budget_error(e)) return;
            const bool failed = st == "failed" || st == "timeout";
            if (!failed && st != "done") return;
            const std::string conv = j.stack[0].conv;
            auto cv = store.conv_load(conv);
            if (!cv) return;
            const std::string title = (*cv)["meta"].value("title", ""), task = j.task_id;
            const std::string link = task.empty() ? "#/chats/" + conv : "#/tasks/" + task;
            if (failed && !task.empty()) { notifier->post({"failed", j.user_id, "Task failed: " + title, e, "Görev başarısız: " + title, e, link, ""}); return; }
            const int minutes = notifier->done_minutes();
            if (minutes < 0 || runner.conversation_busy(conv)) return;  // helpers are still working: their report ends it
            std::string asked;
            const json& msgs = (*cv)["messages"];
            for (auto it = msgs.rbegin(); it != msgs.rend() && asked.empty(); ++it)
                if (it->value("from", "").rfind("user:", 0) == 0) asked = it->value("ts", "");
            if (asked.empty() || (long long)std::time(nullptr) - iso_to_unix(asked) < minutes * 60LL) return;
            auto ghost = store.agent(j.agent_id);
            const std::string name = ghost ? ghost->value("name", "") : j.agent_id;
            if (failed) notifier->post({"failed", j.user_id, name + " could not finish", e, name + " bitiremedi", e, link, ""});
            else notifier->post({"done", j.user_id, name + " finished: " + title, truncate_utf8(a, 300), name + " bitirdi: " + title, truncate_utf8(a, 300), link, ""});
        });
        scripts.set_usage(&usage);
        scripts.set_builtin([&store](const std::string& name, script::Value& out) {
            if (name != "tokens_today") return false;
            long long sum = 0;
            for (auto& [agent, n] : store.tokens_today()) sum += n;
            out = script::Value::number(sum * 100);
            return true;
        });
        scripts.start();
        // Talking to ghosts from Telegram (Settings > MCP > Telegram > listen). Never deleted: a long poll may still be
        // waiting when the program quits; stop() only tells its threads to end.
        auto* telegram = new TelegramBridge(store, auth, runner, mcp, cfg.file.parent_path());
        store.set_append_hook([telegram](const std::string& conv, const json& line) { telegram->on_append(conv, line); });
        telegram->start();
        telegram->set_asks([&scripts](const std::string& user, bool owner) { return scripts.pending_asks(user, owner); },
                           [&scripts](const json& ask, const std::string& user, bool owner, const std::string& by, bool approved, const std::string& value, std::string& err) {
                               return scripts.answer_ask(ask.value("run_id", ""), ask.value("node", ""), user, owner, by, approved, value, err);
                           });
        notifier->set_toast([](const Notice& n) {
            if (!g_hwnd) return;
            const bool tr = g_tr;
            tray_balloon(Widen(tr ? n.title_tr : n.title).c_str(), Widen(tr ? n.text_tr : n.text).c_str());
        });
        notifier->set_telegram([&store, &auth, telegram](const std::string& user, const Notice& n) {
            const std::string en = "🔔 " + n.title + (n.text.empty() ? "" : "\n" + n.text) + (n.link.rfind("#/scripts/", 0) == 0 && n.kind == "ask" ? "\n\nAnswer with /yes, /no or /answer <value>." : "");
            const std::string tr = "🔔 " + n.title_tr + (n.text_tr.empty() ? "" : "\n" + n.text_tr) + (n.link.rfind("#/scripts/", 0) == 0 && n.kind == "ask" ? "\n\n/yes, /no ya da /answer <değer> ile yanıtla." : "");
            if (!user.empty()) { telegram->send_to_user(user, en, tr); return; }
            for (auto& u : store.users()) if (auth.role(u.value("id", "")) == "owner") telegram->send_to_user(u.value("id", ""), en, tr);
        });
        notifier->start();
        g_cfg = &cfg;
        g_store = &store;
        g_runner = &runner;
        g_scripts = &scripts;
        {   // lending this PC's AI was switched on before (in the window), or is asked for on the command line
            std::string url = cli_worker_url.empty() ? cfg.worker_url : cli_worker_url, token = cli_worker_token.empty() ? cfg.worker_token : cli_worker_token, err;
            if (!url.empty() && !g_worker.start(url, token, err)) std::cerr << "Lending the AI: " << err << "\n";
        }
        if (!window_mode) SetConsoleCtrlHandler(on_ctrl, TRUE);

        ServerOptions opt;
        opt.ui_dir = ui_dir;
        opt.config = &cfg;
        Tunnel tunnel;  // starts off
        opt.tunnel = &tunnel;
        opt.telegram = telegram;
        opt.scripts = &scripts;
        opt.usage = &usage;
        opt.notifier = notifier;
        Backup backup(cfg.file.parent_path());
        opt.backup = &backup;
        if (!vault_existed) opt.fresh_vault = cfg.vault;
        std::promise<bool> listening;
        std::atomic<bool> signalled{false};
        std::atomic<HWND> hwnd{nullptr};
        opt.on_listening = [&](bool ok) {
            if (!signalled.exchange(true)) listening.set_value(ok);
            else if (!ok && hwnd.load()) PostMessageW(hwnd.load(), kServerFailed, 0, 0);
        };
        std::thread server([&] {
            bool restart = true;
            while (restart) run_server(store, auth, runner, opt, restart);  // restarts when the sharing switch changes
        });
        bool ok = listening.get_future().get();
        if (!ok) {
            server.join();
            fatal(L"Phantom Co. could not start its local server on port " + std::to_wstring(cfg.port) +
                      L". Another program may be using that port.\nTry the option --port <number>, or change \"port\" in " + cfg.file.wstring() + L".",
                  window_mode);
            return 2;
        }

        int code = 0;
        if (window_mode) {
            hwnd = make_window(inst);
            g_hwnd = hwnd;
            g_home_url = L"http://127.0.0.1:" + std::to_wstring(cfg.port) + L"/";
            WebViewHost host;
            g_host = &host;
            host.on_message = on_page_message;
            std::wstring error;
            if (!host.Create(hwnd, g_home_url, cfg.file.parent_path().wstring(), dev, error)) {
                fatal(error, true);
                code = 1;
            } else {
                g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
                refresh_title();
                tray(NIM_ADD);
                SetTimer(hwnd, kTitleTimer, 5000, nullptr);  // the company name may change in Settings
                ShowWindow(hwnd, SW_SHOW);
                UpdateWindow(hwnd);
                // While the program runs (also hidden) the PC stays awake (ghosts work, friends connect). The screen may
                // still turn off. This only stops sleep when the PC is idle: Start > Sleep, the power button or closing
                // a laptop lid still put it to sleep (Windows power settings decide those).
                SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
                ShutdownBlockReasonCreate(hwnd, L"Phantom Co. is open: your ghosts and friends use this PC. Quit Phantom Co. first, or shut down anyway.");
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            SetThreadExecutionState(ES_CONTINUOUS);  // the program quits: the PC may sleep again
            g_host = nullptr;
            g_worker.stop();
            stop_server();  // "Quit" was chosen: stop serving
        } else {
            std::cout << "Phantom Co. server is running.\n  Vault: " << U8(store.root()) << "\n  Open:  http://127.0.0.1:" << cfg.port << "/\n  Press Ctrl+C to stop.\n";
        }
        server.join();  // headless: returns when Ctrl+C stops the server
        tunnel.Stop();
        telegram->stop();
        notifier->stop();
        g_worker.stop();
        runner.shutdown();
        scripts.shutdown();
        usage.shutdown();
        g_store = nullptr;
        g_runner = nullptr;
        g_scripts = nullptr;
        if (single) ReleaseMutex(single);
        return code;
    } catch (const std::exception& e) {
        fatal(L"Phantom Co. could not start:\n" + Widen(e.what()), window_mode);
        return 1;
    }
}
