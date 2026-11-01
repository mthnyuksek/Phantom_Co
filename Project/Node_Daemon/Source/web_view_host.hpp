#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <set>
#include <string>

namespace pc2 {

// Hosts one WebView2 control (the Edge engine) inside a window and shows the Phantom Co. page in it.
// WebView2.h is huge, so it stays inside the .cpp (pimpl).
//
// Page -> program: window.chrome.webview.postMessage(obj) arrives in on_message (UTF-8 JSON text).
class WebViewHost {
public:
    WebViewHost();
    ~WebViewHost();

    // False with a readable `error` when the WebView2 runtime or the loader is missing.
    // The control appears asynchronously and then loads `url`.
    bool Create(HWND parent, const std::wstring& url, const std::wstring& data_dir, bool dev_keys, std::wstring& error);
    void Navigate(const std::wstring& url);
    void AllowOrigin(const std::wstring& origin);  // pages of these origins may stay in the window
    void Resize(int width, int height);
    void Focus();
    void OpenDevTools();

    std::function<void(const std::string&)> on_message;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pc2
