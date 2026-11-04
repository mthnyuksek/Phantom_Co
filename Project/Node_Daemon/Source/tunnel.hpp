#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "util.hpp"

namespace pc2 {

// "Internet access": a free Cloudflare quick tunnel (cloudflared, no account) that gives this PC's local
// server an https://<random>.trycloudflare.com address. Starts OFF at every program start; the address
// changes every time. cloudflared.exe is not shipped: Install() downloads it from Cloudflare's GitHub release
// into %LOCALAPPDATA%\Phantom_Co\runtime (a copy on PATH is used too).
class Tunnel {
public:
    Tunnel();
    ~Tunnel();
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    static bool Installed();
    void Install();                // downloads in the background
    void Start(int local_port);    // starts cloudflared in the background
    void Stop();
    json status() const;           // {state: off|installing|starting|on|error, installed, url, message}
    std::string host() const;      // "xxx.trycloudflare.com" while on, else ""

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

}  // namespace pc2
