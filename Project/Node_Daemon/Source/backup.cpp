#include "backup.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <regex>
#include <vector>

namespace pc2 {

namespace {

constexpr unsigned long long kFourGig = 0xFFFFFFFFull;

unsigned crc_update(unsigned crc, const unsigned char* p, size_t n) {
    static const std::vector<unsigned> table = [] {
        std::vector<unsigned> t(256);
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct Writer {
    FILE* f = nullptr;
    bool ok = true;
    void bytes(const void* p, size_t n) { if (ok && n && fwrite(p, 1, n, f) != n) ok = false; }
    void u16(unsigned v) { unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; bytes(b, 2); }
    void u32(unsigned long long v) { unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)}; bytes(b, 4); }
    void u64(unsigned long long v) { u32(v & 0xFFFFFFFFull); u32(v >> 32); }
    long long pos() { return _ftelli64(f); }
    void seek(long long p) { if (_fseeki64(f, p, SEEK_SET) != 0) ok = false; }
};

struct Entry {
    std::string name;
    unsigned crc = 0, dos_time = 0, dos_date = 0;
    unsigned long long size = 0, offset = 0;
};

void dos_stamp(const fs::path& p, Entry& e) {
    e.dos_date = 33;  // 1980-01-01
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return;
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st) || st.wYear < 1980) return;
    e.dos_time = (unsigned)st.wHour << 11 | (unsigned)st.wMinute << 5 | st.wSecond / 2;
    e.dos_date = (unsigned)(st.wYear - 1980) << 9 | (unsigned)st.wMonth << 5 | st.wDay;
}

const std::regex& name_rule() {
    static const std::regex r("Phantom_Backup_[0-9-]+_[0-9]+_(full|data)\\.zip");
    return r;
}

}  // namespace

Backup::Backup(const fs::path& data_dir) : dir_(data_dir / L"Backups"), last_(json::object()) {}

Backup::~Backup() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Backup::start(const fs::path& vault, bool include_files, std::string& error) {
    std::lock_guard<std::mutex> l(mu_);
    if (running_) { error = "A backup is already being made."; return false; }
    if (worker_.joinable()) worker_.join();
    std::error_code ec;
    fs::create_directories(dir_, ec);
    running_ = true;
    files_done_ = 0;
    bytes_done_ = 0;
    last_ = json::object();
    worker_ = std::thread([this, vault, include_files] { make(vault, include_files); });
    return true;
}

void Backup::make(fs::path vault, bool include_files) {
    std::time_t now = std::time(nullptr);
    std::tm lt = {};
    localtime_s(&lt, &now);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", &lt);
    const std::string name = std::string("Phantom_Backup_") + stamp + (include_files ? "_full.zip" : "_data.zip");
    const fs::path final_path = dir_ / P(name), part = dir_ / P(name + ".part");

    json result = {{"name", name}, {"skipped", 0}};
    std::vector<Entry> entries;
    Writer w;
    w.f = _wfopen(part.c_str(), L"wb");
    if (!w.f) {
        result["error"] = "The backup file could not be created.";
    } else {
        std::vector<char> buf(1 << 20);
        std::error_code ec;
        long long skipped = 0;
        const fs::path root = fs::absolute(vault, ec);
        auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end && w.ok && !stop_; it.increment(ec)) {
            const fs::path p = it->path();
            std::error_code e2;
            const fs::path rel = p.lexically_relative(root);
            if (rel.empty()) continue;
            const std::wstring top = rel.begin()->wstring();
            if (!include_files && it->is_directory(e2)) {
                if (top == L"Shared_Files") { it.disable_recursion_pending(); continue; }
                auto part_it = rel.begin();
                if (top == L"Projects" && ++part_it != rel.end() && ++part_it != rel.end() && part_it->wstring() != L".Phantom") { it.disable_recursion_pending(); continue; }
            }
            if (!it->is_regular_file(e2)) continue;
            if (rel == fs::path(L".phantom_lock")) continue;
            if (!include_files) {  // loose files at the top of a project folder are work files too
                if (top == L"Shared_Files") continue;
                auto part_it = rel.begin();
                if (top == L"Projects" && ++part_it != rel.end() && ++part_it != rel.end() && part_it->wstring() != L".Phantom") continue;
            }
            FILE* in = _wfopen(p.c_str(), L"rb");
            if (!in) { ++skipped; continue; }

            Entry e;
            e.name = U8(rel);
            std::replace(e.name.begin(), e.name.end(), '\\', '/');
            dos_stamp(p, e);
            e.offset = (unsigned long long)w.pos();
            const unsigned long long expected = fs::file_size(p, e2);
            const bool big = !e2 && expected >= kFourGig;
            w.u32(0x04034b50);
            w.u16(big ? 45 : 20);
            w.u16(0x0800);  // names are UTF-8
            w.u16(0);       // stored
            w.u16(e.dos_time);
            w.u16(e.dos_date);
            const long long crc_at = w.pos();
            w.u32(0);
            w.u32(big ? kFourGig : 0);
            w.u32(big ? kFourGig : 0);
            w.u16((unsigned)e.name.size());
            w.u16(big ? 20 : 0);
            w.bytes(e.name.data(), e.name.size());
            long long extra_at = 0;
            if (big) {
                w.u16(1);
                w.u16(16);
                extra_at = w.pos();
                w.u64(0);
                w.u64(0);
            }
            size_t n;
            while (w.ok && !stop_ && (n = fread(buf.data(), 1, buf.size(), in)) > 0) {
                e.crc = crc_update(e.crc, reinterpret_cast<unsigned char*>(buf.data()), n);
                w.bytes(buf.data(), n);
                e.size += n;
                bytes_done_ += (long long)n;
            }
            fclose(in);
            const long long after = w.pos();
            w.seek(crc_at);
            w.u32(e.crc);
            if (big) {
                w.seek(extra_at);
                w.u64(e.size);
                w.u64(e.size);
            } else if (e.size >= kFourGig) {
                w.ok = false;  // the file grew past 4 GB while it was read
            } else {
                w.u32(e.size);
                w.u32(e.size);
            }
            w.seek(after);
            entries.push_back(std::move(e));
            ++files_done_;
        }
        // central directory
        const unsigned long long cd_start = (unsigned long long)w.pos();
        for (const Entry& e : entries) {
            const bool size64 = e.size >= kFourGig, off64 = e.offset >= kFourGig;
            const unsigned extra = (size64 ? 16 : 0) + (off64 ? 8 : 0);
            w.u32(0x02014b50);
            w.u16(45);
            w.u16(extra ? 45 : 20);
            w.u16(0x0800);
            w.u16(0);
            w.u16(e.dos_time);
            w.u16(e.dos_date);
            w.u32(e.crc);
            w.u32(size64 ? kFourGig : e.size);
            w.u32(size64 ? kFourGig : e.size);
            w.u16((unsigned)e.name.size());
            w.u16(extra ? extra + 4 : 0);
            w.u16(0);
            w.u16(0);
            w.u16(0);
            w.u32(0);
            w.u32(off64 ? kFourGig : e.offset);
            w.bytes(e.name.data(), e.name.size());
            if (extra) {
                w.u16(1);
                w.u16(extra);
                if (size64) { w.u64(e.size); w.u64(e.size); }
                if (off64) w.u64(e.offset);
            }
        }
        const unsigned long long cd_end = (unsigned long long)w.pos(), cd_size = cd_end - cd_start;
        const bool need64 = entries.size() >= 0xFFFF || cd_start >= kFourGig || cd_size >= kFourGig;
        if (need64) {
            w.u32(0x06064b50);
            w.u64(44);
            w.u16(45);
            w.u16(45);
            w.u32(0);
            w.u32(0);
            w.u64(entries.size());
            w.u64(entries.size());
            w.u64(cd_size);
            w.u64(cd_start);
            w.u32(0x07064b50);
            w.u32(0);
            w.u64(cd_end);
            w.u32(1);
        }
        w.u32(0x06054b50);
        w.u16(0);
        w.u16(0);
        w.u16(need64 ? 0xFFFF : (unsigned)entries.size());
        w.u16(need64 ? 0xFFFF : (unsigned)entries.size());
        w.u32(need64 ? kFourGig : cd_size);
        w.u32(need64 ? kFourGig : cd_start);
        w.u16(0);
        const bool closed = fclose(w.f) == 0;
        result["skipped"] = skipped;
        std::error_code e3;
        if (stop_) {
            fs::remove(part, e3);
            result["error"] = "The program was closed during the backup.";
        } else if (!w.ok || !closed || ec) {
            fs::remove(part, e3);
            result["error"] = "The backup could not be finished (is the disk full?).";
        } else {
            fs::rename(part, final_path, e3);
            if (e3) { fs::remove(part, e3); result["error"] = "The backup file could not be saved."; }
        }
    }
    std::lock_guard<std::mutex> l(mu_);
    last_ = result;
    running_ = false;
}

json Backup::status() {
    json out = {{"running", running_.load()}, {"files_done", files_done_.load()}, {"bytes_done", bytes_done_.load()}};
    {
        std::lock_guard<std::mutex> l(mu_);
        out["last"] = last_;
    }
    json list = json::array();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir_, ec)) {
        const std::string name = U8(e.path().filename());
        if (!std::regex_match(name, name_rule())) continue;
        std::error_code e2;
        list.push_back({{"name", name}, {"size", (long long)e.file_size(e2)}, {"full", name.find("_full.zip") != std::string::npos}});
    }
    std::sort(list.begin(), list.end(), [](const json& a, const json& b) { return a.value("name", "") > b.value("name", ""); });
    out["backups"] = list;
    return out;
}

fs::path Backup::file(const std::string& name) {
    if (!std::regex_match(name, name_rule())) return {};
    fs::path p = dir_ / P(name);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? p : fs::path();
}

bool Backup::remove(const std::string& name) {
    fs::path p = file(name);
    if (p.empty()) return false;
    std::error_code ec;
    return fs::remove(p, ec);
}

}  // namespace pc2
