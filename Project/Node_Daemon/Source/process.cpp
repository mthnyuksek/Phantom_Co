#include "process.hpp"

#include <algorithm>
#include <map>

#include "util.hpp"

namespace pc2 {

static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r += L'"';
            backslashes = 0;
        } else {
            r.append(backslashes, L'\\');
            backslashes = 0;
            r += c;
        }
    }
    r.append(backslashes * 2, L'\\');
    r += L'"';
    return r;
}

static bool is_script(const std::wstring& exe) {
    std::wstring l = exe;
    for (auto& c : l) c = (wchar_t)towlower(c);
    return l.ends_with(L".cmd") || l.ends_with(L".bat");
}

std::wstring command_line(const std::string& exe_u8, const std::vector<std::string>& args) {
    std::wstring exe = Widen(exe_u8);
    std::wstring cmd = quote_arg(exe);
    for (auto& a : args) cmd += L" " + quote_arg(Widen(a));
    return is_script(exe) ? L"cmd.exe /d /s /c \"" + cmd + L"\"" : cmd;
}

std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& changes) {
    if (changes.empty()) return L"";
    auto upper = [](std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; };
    std::map<std::wstring, std::wstring> vars;  // UPPER name -> "Name=value"
    if (wchar_t* all = GetEnvironmentStringsW()) {
        for (const wchar_t* p = all; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            size_t eq = kv.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq != std::wstring::npos) vars[upper(kv.substr(0, eq))] = kv;
        }
        FreeEnvironmentStringsW(all);
    }
    for (auto& [name, value] : changes) {
        std::wstring n = Widen(name);
        if (value.empty()) vars.erase(upper(n));
        else vars[upper(n)] = n + L"=" + Widen(value);
    }
    std::wstring block;
    for (auto& [k, kv] : vars) { block += kv; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

Process::~Process() {
    if (stdin_writer_.joinable()) stdin_writer_.join();
    if (out_read_) CloseHandle(out_read_);
    if (process_) CloseHandle(process_);
    if (job_) CloseHandle(job_);  // KILL_ON_JOB_CLOSE ends any leftovers
}

bool Process::start(const ProcessSpec& spec, std::string& error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_w = nullptr, in_r = nullptr, in_w = nullptr;
    if (!CreatePipe(&out_read_, &out_w, &sa, 1 << 20) || !CreatePipe(&in_r, &in_w, &sa, 1 << 20)) {
        error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = command_line(spec.exe, spec.args);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi{};
    std::wstring cwd = Widen(spec.cwd);

    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    std::wstring env = environment_block(spec.env);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.empty() ? nullptr : env.data(),
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ") for " + spec.exe;
        CloseHandle(in_w);
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;

    // Feed the prompt on a separate thread so a full pipe can never block us.
    std::string data = spec.stdin_data;
    stdin_writer_ = std::thread([in_w, data = std::move(data)]() {
        size_t off = 0;
        while (off < data.size()) {
            DWORD w = 0;
            if (!WriteFile(in_w, data.data() + off, (DWORD)std::min<size_t>(data.size() - off, 1 << 16), &w, nullptr) || w == 0) break;
            off += w;
        }
        CloseHandle(in_w);
    });
    return true;
}

void Process::read_lines(const std::function<void(const std::string&)>& on_line) {
    std::string buf;
    char chunk[8192];
    DWORD n = 0;
    while (ReadFile(out_read_, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_line(line);
        }
    }
    if (!buf.empty()) on_line(buf);
}

int Process::wait() {
    if (!process_) return -1;
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return (int)code;
}

void Process::kill() {
    if (job_) TerminateJobObject(job_, 1);
    else if (process_) TerminateProcess(process_, 1);
}

}  // namespace pc2
