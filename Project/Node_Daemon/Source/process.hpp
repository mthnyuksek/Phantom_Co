#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pc2 {

struct ProcessSpec {
    std::string exe;                // full path (UTF-8); .cmd/.bat are run through cmd.exe
    std::vector<std::string> args;  // plain arguments (no shell metacharacters expected)
    std::string cwd;                // working directory (UTF-8)
    std::string stdin_data;         // written to stdin, then stdin is closed
    // Changes to the inherited environment: {name, value}; an empty value removes the variable.
    std::vector<std::pair<std::string, std::string>> env;
};

// The inherited environment with these changes, as a CreateProcessW(CREATE_UNICODE_ENVIRONMENT) block ("" = no change).
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& changes);

// A child process with merged stdout+stderr, owned by a Job Object so that
// kill() (or closing the Node) also terminates every grandchild.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool start(const ProcessSpec& spec, std::string& error);
    void read_lines(const std::function<void(const std::string&)>& on_line);  // until EOF
    int wait();                                                                 // exit code
    void kill();

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE out_read_ = nullptr;
    std::thread stdin_writer_;
};

}  // namespace pc2
