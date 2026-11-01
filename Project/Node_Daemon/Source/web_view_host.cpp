#include "web_view_host.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <vector>

#include "WebView2.h"
#include "embedded.hpp"
#include "util.hpp"

namespace pc2 {
namespace {

// MinGW has no __uuidof for the WebView2 interfaces; these two GUIDs are copied from WebView2.h.
const GUID kIidController2 = {0xc979903e, 0xd4ca, 0x4228, {0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab}};
const GUID kIidSettings3 = {0xfdb5ab74, 0xaf33, 0x4854, {0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba}};

std::string NarrowZ(const wchar_t* s) { return s ? Narrow(std::wstring(s)) : std::string(); }

std::wstring OriginOf(const std::wstring& url) {  // "http://host:port" of an http(s) URL, else ""
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) return L"";
    size_t start = url.find(L"//") + 2, end = url.find_first_of(L"/?#", start);
    return url.substr(0, end == std::wstring::npos ? std::wstring::npos : end);
}

// The loader DLL travels inside the program; it is copied out once so LoadLibrary can use it.
std::wstring ExtractLoader(const std::wstring& data_dir) {
    for (size_t i = 0; i < kEmbeddedCount; ++i) {
        const EmbeddedFile& f = kEmbedded[i];
        if (std::string(f.group) != "loader" || std::string(f.path) != "WebView2Loader.dll") continue;
        std::wstring dir = data_dir + L"\\runtime";
        CreateDirectoryW(data_dir.c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\WebView2Loader.dll";
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow == f.size && info.nFileSizeHigh == 0) return path;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return L"";
        DWORD written = 0;
        BOOL ok = WriteFile(file, f.data, static_cast<DWORD>(f.size), &written, nullptr);
        CloseHandle(file);
        return ok && written == f.size ? path : L"";
    }
    return L"";
}

// A COM callback object that forwards Invoke to a lambda. Reference counted like any COM object.
template <class Iface, class... Args>
class Handler final : public Iface {
public:
    explicit Handler(std::function<HRESULT(Args...)> fn) : fn_(std::move(fn)) {}
    // WebView2 only ever uses the pointer it was handed, so IUnknown is all that has to be answered.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown) { *out = static_cast<Iface*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn_(args...); }

private:
    std::function<HRESULT(Args...)> fn_;
    std::atomic<ULONG> refs_{1};
};

template <class Iface, class... Args, class Fn>
Iface* MakeHandler(Fn fn) { return new Handler<Iface, Args...>(std::function<HRESULT(Args...)>(std::move(fn))); }

template <class T>
void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void OpenInBrowser(const std::wstring& uri) {
    if (uri.rfind(L"http://", 0) == 0 || uri.rfind(L"https://", 0) == 0) ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}  // namespace

struct WebViewHost::Impl {
    WebViewHost* owner = nullptr;
    HWND parent = nullptr;
    HMODULE loader = nullptr;
    ICoreWebView2Environment* env = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* view = nullptr;
    RECT bounds = {0, 0, 0, 0};
    std::wstring start_url, pending_url;
    bool dev_keys = false;
    std::set<std::wstring> allowed;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);  // callbacks check it after the host is gone

    void OnControllerCreated(ICoreWebView2Controller* created);
};

void WebViewHost::Impl::OnControllerCreated(ICoreWebView2Controller* created) {
    controller = created;
    controller->AddRef();
    controller->get_CoreWebView2(&view);
    if (!view) return;

    ICoreWebView2Controller2* controller2 = nullptr;
    if (SUCCEEDED(controller->QueryInterface(kIidController2, reinterpret_cast<void**>(&controller2))) && controller2) {
        COREWEBVIEW2_COLOR dark = {255, 12, 10, 20};  // same as --bg in app.css: no white flash while loading
        controller2->put_DefaultBackgroundColor(dark);
        controller2->Release();
    }
    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(view->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(dev_keys ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        // Browser shortcuts (F5, Ctrl+P ...) are off: it is an app. Developers keep them with --devtools.
        ICoreWebView2Settings3* settings3 = nullptr;
        if (SUCCEEDED(settings->QueryInterface(kIidSettings3, reinterpret_cast<void**>(&settings3))) && settings3) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(dev_keys ? TRUE : FALSE);
            settings3->Release();
        }
        settings->Release();
    }

    const std::shared_ptr<bool> life = alive;
    EventRegistrationToken token;
    view->add_WebMessageReceived(
        MakeHandler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
            [this, life](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                LPWSTR json = nullptr;
                if (*life && SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                    if (owner->on_message) owner->on_message(NarrowZ(json));
                    CoTaskMemFree(json);
                }
                return S_OK;
            }),
        &token);

    // Pages of other sites must never replace the app page; they open in the normal browser instead.
    view->add_NavigationStarting(
        MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            [this, life](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                LPWSTR uri = nullptr;
                if (*life && SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    const std::wstring target = uri;
                    CoTaskMemFree(uri);
                    if (target.rfind(L"about:", 0) != 0 && !allowed.count(OriginOf(target))) {
                        args->put_Cancel(TRUE);
                        OpenInBrowser(target);
                    }
                }
                return S_OK;
            }),
        &token);
    view->add_NewWindowRequested(
        MakeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
            [life](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                args->put_Handled(TRUE);
                LPWSTR uri = nullptr;
                if (*life && SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    OpenInBrowser(uri);
                    CoTaskMemFree(uri);
                }
                return S_OK;
            }),
        &token);

    controller->put_Bounds(bounds);
    controller->put_IsVisible(TRUE);
    view->Navigate((pending_url.empty() ? start_url : pending_url).c_str());
    pending_url.clear();
}

WebViewHost::WebViewHost() : impl_(new Impl) { impl_->owner = this; }

WebViewHost::~WebViewHost() {
    *impl_->alive = false;
    if (impl_->controller) impl_->controller->Close();
    SafeRelease(impl_->view);
    SafeRelease(impl_->controller);
    SafeRelease(impl_->env);
}

bool WebViewHost::Create(HWND parent, const std::wstring& url, const std::wstring& data_dir, bool dev_keys, std::wstring& error) {
    impl_->parent = parent;
    impl_->start_url = url;
    impl_->dev_keys = dev_keys;
    impl_->allowed.insert(OriginOf(url));
    GetClientRect(parent, &impl_->bounds);

    const std::wstring loader_path = ExtractLoader(data_dir);
    if (loader_path.empty()) { error = L"The WebView2 loader (WebView2Loader.dll) could not be unpacked."; return false; }
    impl_->loader = LoadLibraryW(loader_path.c_str());
    typedef HRESULT(STDAPICALLTYPE * CreateEnvironmentFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto create_env = impl_->loader ? reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(impl_->loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
    if (!create_env) { error = L"WebView2Loader.dll could not be loaded."; return false; }

    Impl* impl = impl_.get();
    const std::shared_ptr<bool> life = impl->alive;
    const std::wstring web_dir = data_dir + L"\\webview_data";
    CreateDirectoryW(data_dir.c_str(), nullptr);
    CreateDirectoryW(web_dir.c_str(), nullptr);
    auto* env_done = MakeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
        [impl, life](HRESULT result, ICoreWebView2Environment* created) {
            if (!*life || FAILED(result) || !created) return S_OK;
            impl->env = created;
            created->AddRef();
            created->CreateCoreWebView2Controller(
                impl->parent, MakeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                                  [impl, life](HRESULT r, ICoreWebView2Controller* controller) {
                                      if (*life && SUCCEEDED(r) && controller) impl->OnControllerCreated(controller);
                                      return S_OK;
                                  }));
            return S_OK;
        });
    const HRESULT hr = create_env(nullptr, web_dir.c_str(), nullptr, env_done);
    env_done->Release();
    if (FAILED(hr)) {
        error = L"The Microsoft Edge WebView2 Runtime was not found. It is installed with Windows 11 and current Windows 10; "
                L"otherwise install it from Microsoft (\"WebView2 Runtime\").";
        return false;
    }
    return true;
}

void WebViewHost::Navigate(const std::wstring& url) {
    impl_->allowed.insert(OriginOf(url));
    if (impl_->view) impl_->view->Navigate(url.c_str());
    else impl_->pending_url = url;
}
void WebViewHost::AllowOrigin(const std::wstring& origin) { impl_->allowed.insert(origin); }

void WebViewHost::Resize(int width, int height) {
    impl_->bounds = {0, 0, width, height};
    if (impl_->controller) impl_->controller->put_Bounds(impl_->bounds);
}
void WebViewHost::Focus() { if (impl_->controller) impl_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
void WebViewHost::OpenDevTools() { if (impl_->view) impl_->view->OpenDevToolsWindow(); }
void WebViewHost::PostToPage(const std::string& json_text) {
    if (impl_->view) impl_->view->PostWebMessageAsJson(Widen(json_text).c_str());
}

}  // namespace pc2
