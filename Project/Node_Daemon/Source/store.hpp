#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "md.hpp"
#include "util.hpp"

namespace pc2 {

// Local (per-PC) settings, stored outside the Vault and never synced.
struct Config {
    fs::path file;   // node.json
    fs::path vault;  // Vault folder
    int port = 7777;
    bool share = false;  // listen on the network (all addresses) instead of this PC only
    std::string worker_url, worker_token;  // lend this PC's Claude/Codex to that company (empty = off)
    bool check_updates = true;  // ask GitHub for a newer release (Settings > About)
};

fs::path default_vault_path();  // <Documents>\Phantom_Vault  (visible on purpose)
fs::path config_dir();          // %LOCALAPPDATA%\Phantom_Co
fs::path config_file_path();
Config load_config(const fs::path& file = {});  // empty = config_file_path()
void save_config(const Config& c);
// The Vault folder for a folder the person picked: the folder itself when it is empty, missing or already a
// Vault, otherwise a "Phantom_Vault" folder inside it (a Vault must not be spread over somebody's Documents).
fs::path vault_target(const fs::path& picked);
bool same_path(const fs::path& a, const fs::path& b);  // case-insensitive, ignores a trailing slash
bool path_inside(const fs::path& child, const fs::path& parent);  // child lies somewhere below parent (case-insensitive)

// Creates the Vault (and its starting layout from the embedded template) when it does
// not exist, upgrades a version 1 Vault, validates it otherwise. Never overwrites user
// files. Throws std::runtime_error with a user-readable message.
void init_vault(const fs::path& root);

// Vault layout (version 2) - every entity is a Markdown file, see Development/Docs/vault_format.md
class Store {
public:
    explicit Store(const fs::path& root);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    fs::path root() const;
    fs::path files_root() const;                    // Shared_Files
    fs::path project_dir(const std::string& id) const;
    void switch_root(const fs::path& new_root);

    json company();
    void set_company(const json& c);
    json settings();                                // with defaults filled in
    void set_settings(const json& patch);
    int setting_int(const char* name);

    std::vector<json> users();
    std::optional<json> user(const std::string& id);
    json save_user(json u);                         // assigns id from the name when missing
    bool remove_user(const std::string& id);

    std::vector<json> agents();
    std::optional<json> agent(const std::string& id);
    json save_agent(json a);
    bool remove_agent(const std::string& id);

    std::vector<json> skills();
    std::optional<json> skill(const std::string& id);
    json save_skill(json s);
    bool remove_skill(const std::string& id);
    std::vector<json> skill_templates();            // built into the program
    std::optional<json> install_skill_template(const std::string& template_id);

    std::vector<json> rules();
    std::optional<json> rule(const std::string& id);
    json save_rule(json r);
    bool remove_rule(const std::string& id);

    std::vector<json> projects();                   // each with its notes
    std::optional<json> project(const std::string& id);
    json save_project(json p);
    bool remove_project(const std::string& id);     // removes the .Phantom metadata only, never the files
    json save_note(const std::string& project_id, json n);
    bool remove_note(const std::string& project_id, const std::string& note_id);
    // What one agent does in one project (.Phantom/Roles/<agent id>.md); empty text removes it.
    bool set_project_role(const std::string& project_id, const std::string& agent_id, const std::string& text);

    std::vector<json> tasks();                      // newest first
    std::optional<json> task(const std::string& id);
    json save_task(json t);
    bool remove_task(const std::string& id);

    std::vector<json> task_templates();             // Task_Templates/<id>.md, by name
    std::optional<json> task_template(const std::string& id);
    json save_task_template(json t);
    bool remove_task_template(const std::string& id);

    // Conversations: Conversations/<id>.jsonl, one JSON object per line
    //   {"t":"meta",...} {"t":"msg","from":"user:ID|agent:ID|system","text":..} {"t":"title"} {"t":"share"}
    //   {"t":"session","agent":..,"sid":..} {"t":"reset"}
    std::string conv_create(json meta);
    void conv_append(const std::string& id, json line);
    std::optional<json> conv_load(const std::string& id);  // {meta, messages, sessions}
    std::vector<json> conv_list();
    bool conv_delete(const std::string& id);

    // Runs: Runs/<id>.md (record) + Runs/<id>.jsonl (event log)
    void run_save(const json& rec);
    std::optional<json> run_get(const std::string& id);
    std::vector<json> runs();                       // newest first, without result text
    fs::path run_events_path(const std::string& id);
    void recover_interrupted();

private:
    std::string unique_id(const fs::path& dir, const std::string& name, bool folder);
    void load_run_index();
    void* lock_ = nullptr;  // open handle of <Vault>\.phantom_lock: one Phantom Co. window per Vault

    mutable std::recursive_mutex mu_;
    fs::path root_;
    std::map<std::string, json> run_index_;
};

}  // namespace pc2
