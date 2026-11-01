#include "tunnel.hpp"

#include <windows.h>

#include <thread>

#include "http_client.hpp"
#include "store.hpp"

namespace pc2 {

namespace {
constexpr size_t kMinExeBytes = 10 * 1024 * 1024;  // the real file is about 40 MB: anything smaller is an error page

fs::path runtime_dir() { return config_dir() / L"runtime"; }

std::string exe_path() {
    fs::path own = runtime_dir() / L"cloudflared.exe";
    std::error_code ec;
    if (fs::exists(own, ec)) return U8(own);
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"cloudflared.exe", nullptr, MAX_PATH, found, nullptr) > 0) return Narrow(found);
    return "";
}

// "...https://quiet-river-1234.trycloudflare.com..." -> the address; "" if the text has none.
std::string find_url(const std::string& text) {
    const std::string suffix = ".trycloudflare.com";
    size_t from = 0;
    while (true) {
        size_t end = text.find(suffix, from);
        if (end == std::string::npos) return "";
        from = end + suffix.size();
        size_t start = text.rfind("https://", end);
        if (start == std::string::npos) continue;
        std::string host = text.substr(start + 8, end - start - 8);
        if (host.empty() || host == "api" || host.find_first_of(" \t\r\n/|") != std::string::npos) continue;
        return text.substr(start, end + suffix.size() - start);
    }
}
}  // namespace

struct Tunnel::Shared {
    mutable std::mutex mu;
    std::string state = "off", url, message;
    HANDLE process = nullptr, job = nullptr;
    unsigned generation = 0;  // a reader thread of an older run must not touch the state

    void set(const std::string& s, const std::string& u, const std::string& m) { state = s; url = u; message = m; }
    void kill_locked() {
        if (process) { TerminateProcess(process, 0); CloseHandle(process); process = nullptr; }
        if (job) { CloseHandle(job); job = nullptr; }  // KILL_ON_JOB_CLOSE ends the whole tree
    }
};

Tunnel::Tunnel() : shared_(std::make_shared<Shared>()) {}
Tunnel::~Tunnel() { Stop(); }
bool Tunnel::Installed() { return !exe_path().empty(); }

json Tunnel::status() const {
    std::lock_guard<std::mutex> lk(shared_->mu);
    return {{"state", shared_->state}, {"installed", Installed()}, {"url", shared_->url}, {"message", shared_->message}};
}

std::string Tunnel::host() const {
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (shared_->state != "on" || shared_->url.size() < 9) return "";
    return shared_->url.substr(8);
}

void Tunnel::Install() {
    {
        std::lock_guard<std::mutex> lk(shared_->mu);
        if (shared_->state == "installing") return;
        shared_->set("installing", "", "Downloading cloudflared (about 40 MB)...");
    }
    std::thread([shared = shared_] {
        std::string error;
        bool ok = false;
        {
            HttpClient cli("https://github.com", 10000, 120000);
            auto r = cli.Get("/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe");
            if (!r) error = "cloudflared could not be downloaded (check the internet connection).";
            else if (r->status != 200) error = "The download server answered with an error (" + std::to_string(r->status) + ").";
            else if (r->body.size() < kMinExeBytes || r->body.compare(0, 2, "MZ") != 0) error = "The downloaded file does not look like cloudflared.";
            else {
                std::error_code ec;
                fs::create_directories(runtime_dir(), ec);
                fs::path target = runtime_dir() / L"cloudflared.exe", part = runtime_dir() / L"cloudflared.exe.part";
                ok = write_file_atomic(part, r->body);
                if (ok) { fs::remove(target, ec); ok = MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; }
                if (!ok) error = "cloudflared could not be saved.";
            }
        }
        std::lock_guard<std::mutex> lk(shared->mu);
        if (shared->state == "installing") shared->set(ok ? "off" : "error", "", ok ? "cloudflared is installed." : error);
    }).detach();
}

void Tunnel::Start(int local_port) {
    Stop();
    std::string exe = exe_path();
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (exe.empty()) { shared_->set("error", "", "cloudflared is not installed."); return; }

    SECURITY_ATTRIBUTES inherit = {sizeof inherit, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) { shared_->set("error", "", "The tunnel could not be started."); return; }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = INVALID_HANDLE_VALUE;
    std::wstring command = L"\"" + Widen(exe) + L"\" tunnel --no-autoupdate --url http://127.0.0.1:" + std::to_wstring(local_port);
    PROCESS_INFORMATION pi = {};
    BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!started) { CloseHandle(read_end); shared_->set("error", "", "cloudflared could not be started."); return; }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    shared_->process = pi.hProcess;
    shared_->job = job;
    unsigned generation = ++shared_->generation;
    shared_->set("starting", "", "Opening the tunnel...");

    std::thread([shared = shared_, read_end, generation] {
        std::string pending, tail;
        char buf[2048];
        DWORD n = 0;
        while (ReadFile(read_end, buf, sizeof buf, &n, nullptr) && n > 0) {
            pending.append(buf, n);
            std::lock_guard<std::mutex> lk(shared->mu);
            if (shared->generation != generation) break;
            if (shared->state == "starting") {
                std::string url = find_url(pending);
                if (!url.empty()) shared->set("on", url, "");
            }
            if (pending.size() > 8192) pending.erase(0, pending.size() - 4096);
            tail = pending.size() > 300 ? pending.substr(pending.size() - 300) : pending;
        }
        CloseHandle(read_end);
        std::lock_guard<std::mutex> lk(shared->mu);
        if (shared->generation == generation && shared->state != "off") {
            shared->kill_locked();
            shared->set("error", "", "The tunnel closed. " + tail);
        }
    }).detach();
}

void Tunnel::Stop() {
    std::lock_guard<std::mutex> lk(shared_->mu);
    ++shared_->generation;
    shared_->kill_locked();
    if (shared_->state != "installing") shared_->set("off", "", "");
}

}  // namespace pc2
