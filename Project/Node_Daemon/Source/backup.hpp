#pragma once
// Vault backups: <data>\Backups\Phantom_Backup_<date>_<full|data>.zip, made on a background thread (Settings > Backup, owner).
// "data" leaves out the work files of projects and Shared_Files. Accounts and keys live outside the Vault, so a backup never holds them.
// The zip is stored without compression (UTF-8 names, Zip64 for very large backups): no library, and any unzip tool opens it.
#include <atomic>
#include <mutex>
#include <thread>

#include "util.hpp"

namespace pc2 {

class Backup {
public:
    explicit Backup(const fs::path& data_dir);
    ~Backup();

    bool start(const fs::path& vault, bool include_files, std::string& error);
    json status();  // {running, files_done, bytes_done, last: {name, error, skipped}, backups: [{name, size, created, full}]}
    fs::path file(const std::string& name);  // empty = not a backup of ours
    bool remove(const std::string& name);

private:
    void make(fs::path vault, bool include_files);

    fs::path dir_;
    std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<long long> files_done_{0}, bytes_done_{0};
    json last_;
};

}  // namespace pc2
