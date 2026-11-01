#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <thread>

#include "auth.hpp"
#include "runner.hpp"
#include "server.hpp"
#include "store.hpp"
#include "tunnel.hpp"
#include "web_view_host.hpp"
#include "worker.hpp"

using namespace pc2;

namespace {
constexpr wchar_t kWindowClass[] = L"PhantomCoWindow";
constexpr UINT kServerFailed = WM_APP + 1;

WebViewHost* g_host = nullptr;
WorkerClient g_worker;
Config* g_cfg = nullptr;
std::wstring g_home_url;

BOOL WINAPI on_ctrl(DWORD) {
    stop_server();
    return TRUE;
}

void fatal(const std::wstring& msg, bool window_mode) {
    if (window_mode) MessageBoxW(nullptr, msg.c_str(), L"Phantom Co.", MB_OK | MB_ICONERROR);
    else std::wcerr << msg << L"\n";
}

void usage() {
    std::cout << "Phantom Co. - ghost company\n"
                 "  --vault <folder>   Vault folder for this run (default: saved setting, then Documents\\PC2_Vault)\n"
                 "  --port <number>    Port of the local server (default 7777)\n"
                 "  --share            Let people on the network connect (the owner can also switch it in Settings)\n"
                 "  --data <folder>    Folder for accounts and settings (default %LOCALAPPDATA%\\PC2)\n"
                 "  --no-window        Run only the server, without a window (tests, a PC without a screen)\n"
                 "  --worker-url <address> --worker-token <token>\n"
                 "                     Lend this PC's Claude/Codex to the company at that address (normally switched on in the window)\n"
                 "  --devtools         Keep browser shortcuts and F12 in the window (development)\n"
                 "  --ui-dir <folder>  Read the web pages from disk instead of the built-in copy (development)\n";
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
    if (cmd == "worker_status") {
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
        case kServerFailed:
            MessageBoxW(hwnd, L"Phantom Co. could not start its local server. Another copy of Phantom Co. may already be running, or the port is in use.\n\nTry the option --port <number>.", L"Phantom Co.", MB_OK | MB_ICONERROR);
            DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
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
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    int w = 1360, h = 860;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Phantom Co. · Ghost Company", WS_OVERLAPPEDWINDOW, std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    return hwnd;
}
}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // Started from a terminal? Then --help and errors can be printed there.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Config cfg = load_config();
    fs::path vault_override, ui_dir, data_dir;
    bool window_mode = true, share = false, dev = false;
    std::string cli_worker_url, cli_worker_token;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--vault") vault_override = next();
        else if (a == L"--port") cfg.port = _wtoi(next().c_str());
        else if (a == L"--ui-dir") ui_dir = next();
        else if (a == L"--data") data_dir = next();
        else if (a == L"--no-window") window_mode = false;
        else if (a == L"--share") share = true;
        else if (a == L"--devtools") dev = true;
        else if (a == L"--worker-url") cli_worker_url = Narrow(next());
        else if (a == L"--worker-token") cli_worker_token = Narrow(next());
        else { usage(); return a == L"--help" ? 0 : 1; }
    }
    if (!data_dir.empty()) cfg.file = fs::absolute(data_dir) / L"node.json";  // an isolated account store (tests, a second company)
    if (!vault_override.empty()) cfg.vault = fs::absolute(vault_override);
    if (share) cfg.share = true;

    // One copy per port: a second start just brings the first window to the front.
    SetLastError(0);  // earlier calls may have left ERROR_ALREADY_EXISTS behind
    HANDLE single = CreateMutexW(nullptr, TRUE, (L"Local\\PhantomCo_Node_" + std::to_wstring(cfg.port)).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
            return 0;
        }
        fatal(L"Phantom Co. is already running on this port.", window_mode);
        return 3;
    }

    try {
        Store store(cfg.vault);
        Auth auth(cfg.file.parent_path() / L"auth.json");
        Runner runner(store);
        g_cfg = &cfg;
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
            fatal(L"Phantom Co. could not start its local server. Another copy of Phantom Co. may already be running, or the port is in use.\nTry the option --port <number>.", window_mode);
            return 2;
        }

        int code = 0;
        if (window_mode) {
            hwnd = make_window(inst);
            g_home_url = L"http://127.0.0.1:" + std::to_wstring(cfg.port) + L"/";
            WebViewHost host;
            g_host = &host;
            host.on_message = on_page_message;
            std::wstring error;
            if (!host.Create(hwnd, g_home_url, cfg.file.parent_path().wstring(), dev, error)) {
                fatal(error, true);
                code = 1;
            } else {
                ShowWindow(hwnd, SW_SHOW);
                UpdateWindow(hwnd);
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            g_host = nullptr;
            g_worker.stop();
            stop_server();  // the window was closed: stop serving
        } else {
            std::cout << "Phantom Co. server is running.\n  Vault: " << U8(store.root()) << "\n  Open:  http://127.0.0.1:" << cfg.port << "/\n  Press Ctrl+C to stop.\n";
        }
        server.join();  // headless: returns when Ctrl+C stops the server
        tunnel.Stop();
        g_worker.stop();
        runner.shutdown();
        if (single) ReleaseMutex(single);
        return code;
    } catch (const std::exception& e) {
        fatal(L"Phantom Co. could not start:\n" + Widen(e.what()), window_mode);
        return 1;
    }
}
