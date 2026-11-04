#include "worker.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <regex>
#include <set>
#include <vector>

#include "http_client.hpp"
#include "process.hpp"
#include "providers.hpp"
#include "store.hpp"
#include "version.hpp"

namespace pc2 {

using namespace std::chrono_literals;

static constexpr int64_t kFileMax = 50ll * 1024 * 1024;  // the host skips bigger files too

static std::string pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

bool WorkerClient::start(const std::string& base_url, const std::string& token, std::string& error) {
    stop();
    bool ok = (base_url.rfind("http://", 0) == 0 || base_url.rfind("https://", 0) == 0) && base_url.size() < 300 && base_url.find_first_of(" \t\r\n\"<>") == std::string::npos;
    if (!ok) { error = "The address of the company must start with http:// or https://."; return false; }
    if (token.size() != 48) { error = "That is not a valid worker token."; return false; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        url_ = base_url;
        while (!url_.empty() && url_.back() == '/') url_.pop_back();
        token_ = token;
        status_ = "connecting";
    }
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this] { loop(); running_ = false; });
    return true;
}

void WorkerClient::stop() {
    stopping_ = true;
    if (void* p = current_proc_.load()) static_cast<Process*>(p)->kill();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    set_status("off");
}

void WorkerClient::set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = s;
}

json WorkerClient::info() {
    std::lock_guard<std::mutex> lk(mu_);
    return {{"running", running_.load()}, {"url", url_}, {"status", status_}, {"providers", providers_}};
}

void WorkerClient::loop() {
    std::string url, token;
    { std::lock_guard<std::mutex> lk(mu_); url = url_; token = token_; }
    auto sleep_s = [&](int s) { for (int i = 0; i < s * 10 && !stopping_; ++i) std::this_thread::sleep_for(100ms); };

    HttpClient cli(url, 10000, 45000);  // the host holds a poll open for 20 s
    Headers h = {{"Authorization", "Bearer " + token}, {"X-PC2", "1"}};

    while (!stopping_) {
        json provs = json::array();
        for (auto& p : detect_providers()) if (p.available && (p.id == "claude" || p.id == "codex")) provs.push_back(p.id);
        { std::lock_guard<std::mutex> lk(mu_); providers_ = provs; }
        if (provs.empty()) { set_status("Neither Claude Code nor Codex was found on this PC."); sleep_s(10); continue; }

        auto r = cli.Post("/api/worker/poll", h, dumps({{"providers", provs}, {"version", PC2_VERSION_STR}, {"protocol", PC2_WORKER_PROTOCOL}}), "application/json");
        if (!r) { set_status("Cannot reach the company. Retrying..."); sleep_s(5); continue; }
        if (r->status == 401) { set_status("The company does not accept this sign-in any more. Switch it on again."); break; }
        json j = json::parse(r->body, nullptr, false);
        // Both sides check the worker protocol: a host older than 1.0 sends no number and is taken as protocol 1.
        bool other_protocol = r->status == 426 ||
                              (r->status == 200 && j.is_object() && j.contains("protocol") && j["protocol"].is_number_integer() && j["protocol"] != PC2_WORKER_PROTOCOL);
        if (other_protocol) {
            std::string host_v = j.is_object() && j.contains("version") && j["version"].is_string() ? j["version"].get<std::string>() : "another version";
            set_status("Different versions: the company runs Phantom Co. " + host_v + ", this PC runs " PC2_VERSION_STR ". Use the same version on both PCs.");
            break;
        }
        if (r->status != 200) { set_status("The company answered with an error (" + std::to_string(r->status) + ")."); sleep_s(5); continue; }
        if (!j.is_object() || !j.contains("job") || !j["job"].is_object()) { set_status("Online, waiting for work"); continue; }
        set_status("Working for the company");
        try {
            run_job(j["job"]);
        } catch (const std::exception& e) {
            set_status(std::string("Error: ") + e.what());
        }
        if (!stopping_) set_status("Online, waiting for work");
    }
}

// ----------------------------------------------------------------- one job
void WorkerClient::run_job(const json& job) {
    std::string url, token;
    { std::lock_guard<std::mutex> lk(mu_); url = url_; token = token_; }
    const std::string run_id = job.value("run_id", "");
    if (!valid_id(run_id)) return;
    Headers h = {{"Authorization", "Bearer " + token}, {"X-PC2", "1"}};
    auto client = [&] { return std::make_unique<HttpClient>(url, 10000, 60000); };
    auto cli = client();
    const std::string q = "?run_id=" + run_id;

    auto finish_with = [&](json result) { client()->Post("/api/worker/finish", h, dumps({{"run_id", run_id}, {"result", result}}), "application/json"); };
    auto fail_with = [&](const std::string& why) { finish_with({{"status", "failed"}, {"error", why}}); };

    // ---- validate what the host sent: it ends up in a command line
    const std::string provider = job.value("provider", "");
    const std::string model = job.value("model", ""), session = job.value("session_id", "");
    if (provider != "claude" && provider != "codex") return fail_with("This PC cannot run " + provider + ".");
    if (!std::regex_match(model, std::regex("[A-Za-z0-9._:/\\[\\]-]{0,80}"))) return fail_with("The model name is not allowed.");
    if (!std::regex_match(session, std::regex("[A-Za-z0-9._:-]{0,120}"))) return fail_with("The session id is not allowed.");
    static const std::set<std::string> allowed_tools = {"Read", "Write", "Edit", "Glob", "Grep", "WebSearch", "WebFetch"};
    RunSpec spec;
    spec.provider = provider;
    spec.model = model;
    spec.session_id = session;
    if (job.contains("tools") && job["tools"].is_array())
        for (auto& t : job["tools"]) if (t.is_string() && allowed_tools.count(t.get<std::string>())) spec.tools.push_back(t.get<std::string>());
    spec.can_write = std::find(spec.tools.begin(), spec.tools.end(), "Write") != spec.tools.end() ||
                     std::find(spec.tools.begin(), spec.tools.end(), "Edit") != spec.tools.end();
    spec.max_turns = std::clamp(job.value("max_turns", 40), 1, 100);
    int timeout_min = std::clamp(job.value("timeout_min", 30), 1, 240);

    // ---- mirror folder: %LOCALAPPDATA%\Phantom_Co\Worker\<host>\<scope>
    std::string host_key = url.substr(url.find("//") + 2), scope = job.value("scope", "shared");
    for (char& c : host_key) if (!isalnum((unsigned char)c)) c = '_';
    for (char& c : scope) if (!isalnum((unsigned char)c)) c = '_';
    fs::path mirror = config_dir() / L"Worker" / P(host_key) / P(scope);
    std::error_code ec;
    fs::create_directories(mirror, ec);
    const fs::path mirror_canon = fs::weakly_canonical(mirror, ec);
    auto local_path = [&](const std::string& rel) -> fs::path {
        if (rel.empty() || rel.find("..") != std::string::npos || rel[0] == '/' || rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos) return {};
        fs::path p = mirror_canon / P(rel);
        auto r = p.lexically_normal().lexically_relative(mirror_canon);
        return r.empty() || *r.begin() == L".." ? fs::path{} : p;
    };

    // ---- queue of events for the host + heartbeat; the host answers with "cancel" when somebody pressed stop
    std::mutex em;
    std::vector<json> pending;
    std::atomic<bool> cancel{false}, flusher_done{false};
    Process* procp = nullptr;
    auto push = [&](json ev) { std::lock_guard<std::mutex> lk(em); pending.push_back(std::move(ev)); };
    auto flush = [&](HttpClient& c) {
        std::vector<json> batch;
        { std::lock_guard<std::mutex> lk(em); batch.swap(pending); }
        for (size_t i = 0; i < batch.size() || i == 0; i += 100) {
            json evs = json::array();
            for (size_t k = i; k < batch.size() && k < i + 100; ++k) evs.push_back(batch[k]);
            auto r = c.Post("/api/worker/event", h, dumps({{"run_id", run_id}, {"events", evs}}), "application/json");
            if (r && r->status == 200) {
                json j = json::parse(r->body, nullptr, false);
                if (j.is_object() && j.value("cancel", false)) cancel = true;
            }
            if (batch.empty()) break;
        }
    };
    std::thread flusher([&] {
        auto c = client();
        auto last = std::chrono::steady_clock::now();
        while (!flusher_done) {
            std::this_thread::sleep_for(300ms);
            bool has;
            { std::lock_guard<std::mutex> lk(em); has = !pending.empty(); }
            auto now = std::chrono::steady_clock::now();
            if (has || now - last > 15s) { flush(*c); last = now; }
            if (cancel && procp) procp->kill();
            if (stopping_) cancel = true;
        }
    });
    auto stop_flusher = [&] { flusher_done = true; if (flusher.joinable()) flusher.join(); };

    push({{"kind", "status"}, {"text", "Preparing the folder on the friend's PC"}});

    // ---- 1. host -> mirror
    std::map<std::string, std::string> base;  // path -> hash right after the sync
    {
        auto r = cli->Get("/api/worker/files" + q, h);
        json list = r && r->status == 200 ? json::parse(r->body, nullptr, false) : json();
        if (!list.is_object() || !list["files"].is_array()) { stop_flusher(); return fail_with("The files of the project could not be listed."); }
        std::set<std::string> wanted;
        for (auto& f : list["files"]) {
            std::string rel = f.value("path", ""), hash = f.value("hash", "");
            fs::path lp = local_path(rel);
            if (lp.empty()) continue;
            wanted.insert(rel);
            std::string cur;
            if (fs::is_regular_file(lp, ec) && (int64_t)fs::file_size(lp, ec) == f.value("size", (int64_t)-1)) {
                std::string data;
                if (read_file(lp, data)) cur = sha256_hex_of(data);
            }
            if (cur != hash) {
                auto g = cli->Get("/api/worker/file" + q + "&path=" + pct(rel), h);
                if (!g || g->status != 200) { stop_flusher(); return fail_with("The file " + rel + " could not be fetched."); }
                fs::create_directories(lp.parent_path(), ec);
                write_file_atomic(lp, g->body);
                cur = sha256_hex_of(g->body);
            }
            base[rel] = cur;
        }
        for (auto it = fs::recursive_directory_iterator(mirror_canon, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {  // the mirror must equal the host
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string rel = Narrow(it->path().lexically_relative(mirror_canon).generic_wstring());
            if (!wanted.count(rel)) fs::remove(it->path(), ec);
        }
    }

    // ---- 2. run the CLI in the mirror
    spec.workdir = U8(mirror_canon);
    std::string wd = spec.workdir;
    spec.system_prompt = replace_all(job.value("system_prompt", ""), "@@WORKDIR@@", wd);
    spec.prompt = replace_all(job.value("prompt", ""), "@@WORKDIR@@", wd);
    fs::path tmp = config_dir() / L"Tmp";
    fs::create_directories(tmp, ec);
    fs::path sysfile = tmp / P("w_" + run_id + ".system.md");
    if (provider == "claude") { write_file_atomic(sysfile, spec.system_prompt); spec.system_file = U8(sysfile); }

    ProcessSpec ps;
    std::string error;
    json result = {{"status", "failed"}};
    LineParser parser(provider);
    int exit_code = 0;
    bool timed_out = false;
    const auto started_at = fs::file_time_type::clock::now() - std::chrono::seconds(2);
    if (!build_command(spec, ps, error)) {
        result["error"] = error;
    } else {
        Process proc;
        if (!proc.start(ps, error)) {
            result["error"] = error;
        } else {
            procp = &proc;
            current_proc_ = &proc;
            if (cancel || stopping_) proc.kill();
            std::mutex wm;
            std::condition_variable wcv;
            bool wdone = false;
            std::thread watchdog([&] {
                std::unique_lock<std::mutex> lk(wm);
                if (!wcv.wait_for(lk, std::chrono::minutes(timeout_min), [&] { return wdone; })) { timed_out = true; proc.kill(); }
            });
            proc.read_lines([&](const std::string& line) { for (auto& ev : parser.feed(line)) push(std::move(ev)); });
            exit_code = proc.wait();
            { std::lock_guard<std::mutex> lk(wm); wdone = true; }
            wcv.notify_all();
            watchdog.join();
            current_proc_ = nullptr;
            procp = nullptr;
        }
    }
    fs::remove(sysfile, ec);

    // ---- 3. mirror -> host (new and changed files only)
    int sent = 0;
    if (!cancel && !stopping_ && error.empty()) {
        push({{"kind", "status"}, {"text", "Sending the changed files back"}});
        if (provider == "codex" && spec.can_write)  // Codex saves images outside the mirror: bring them in so they travel back
            parser.result_text += images_note(collect_codex_images(parser.session_id, started_at, mirror_canon));
        for (auto it = fs::recursive_directory_iterator(mirror_canon, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if ((int64_t)it->file_size(ec) > kFileMax) continue;
            std::string rel = Narrow(it->path().lexically_relative(mirror_canon).generic_wstring());
            std::string data;
            if (!read_file(it->path(), data)) continue;
            std::string hash = sha256_hex_of(data);
            auto b = base.find(rel);
            if (b != base.end() && b->second == hash) continue;
            auto r = cli->Put("/api/worker/file" + q + "&path=" + pct(rel), h, data, "application/octet-stream");
            if (r && r->status == 200) ++sent;
            else push({{"kind", "log"}, {"text", "Could not send " + rel + " back."}});
        }
        if (sent) push({{"kind", "log"}, {"text", std::to_string(sent) + " file(s) were sent back to the company."}});
    }
    stop_flusher();
    { auto c = client(); flush(*c); }

    // ---- 4. result
    if (error.empty()) {
        result["exit_code"] = exit_code;
        result["text"] = parser.result_text;
        result["session_id"] = parser.session_id;
        result["tokens_in"] = parser.tokens_in;
        result["tokens_out"] = parser.tokens_out;
        result["cost"] = parser.cost_usd;
        result["max_turns"] = parser.max_turns;
        if (cancel || stopping_) result["status"] = "cancelled";
        else if (timed_out) { result["status"] = "timeout"; result["error"] = "Stopped after the " + std::to_string(timeout_min) + " minute limit."; }
        else if (parser.max_turns) result["status"] = "done";
        else if (exit_code != 0 || parser.is_error) {
            result["status"] = "failed";
            result["error"] = parser.is_error && !parser.result_text.empty() ? parser.result_text : "The CLI exited with code " + std::to_string(exit_code) + ".";
        } else result["status"] = "done";
    } else {
        result["error"] = error;
    }
    finish_with(result);
}

}  // namespace pc2
