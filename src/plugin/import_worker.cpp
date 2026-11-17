#include "import_worker.h"

#include "state_codec.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace mpcboy {

namespace {

constexpr size_t kMaxScanEntries = 4096;   // bound on a hostile directory

bool endsWithSav(const std::string& n)
{
    if (n.size() < 5) return false;   // ".sav" alone is not a file name we accept
    const char* e = n.c_str() + n.size() - 4;
    return e[0] == '.' && (e[1] | 0x20) == 's' && (e[2] | 0x20) == 'a' && (e[3] | 0x20) == 'v';
}

bool nameLess(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        const unsigned char lx = (x >= 'A' && x <= 'Z') ? static_cast<unsigned char>(x + 32) : x;
        const unsigned char ly = (y >= 'A' && y <= 'Z') ? static_cast<unsigned char>(y + 32) : y;
        if (lx != ly) return lx < ly;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;   // same letters, different case: bytewise
}

}  // namespace

ImportWorker::ImportWorker(std::string dir, ContentStore& store) : dir_(std::move(dir)), store_(store)
{
    scanRequested_.store(true, std::memory_order_relaxed);   // the initial scan
    thread_ = std::thread([this] { run(); });
}

ImportWorker::~ImportWorker()
{
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

bool ImportWorker::requestImport(int index)
{
    if (index < 0) return false;
    int expected = -1;
    return importIdx_.compare_exchange_strong(expected, index, std::memory_order_acq_rel);
}

std::string ImportWorker::lastImportedName() const
{
    std::lock_guard<std::mutex> g(mu_);
    return lastImported_;
}

void ImportWorker::setLastImportedName(const std::string& name)
{
    std::lock_guard<std::mutex> g(mu_);
    lastImported_ = name.size() > kStateMaxName ? name.substr(0, kStateMaxName) : name;
}

void ImportWorker::setPreferredName(const std::string& name)
{
    {
        std::lock_guard<std::mutex> g(mu_);
        preferred_ = name;
    }
    if (!name.empty()) resolveRequested_.store(true, std::memory_order_release);
}

int ImportWorker::fileCount() const
{
    std::lock_guard<std::mutex> g(mu_);
    return static_cast<int>(shared_.size());
}

std::string ImportWorker::fileName(int index) const
{
    std::lock_guard<std::mutex> g(mu_);
    if (index < 0 || index >= static_cast<int>(shared_.size())) return std::string();
    return shared_[static_cast<size_t>(index)];
}

std::string ImportWorker::displayText(int index) const
{
    const int err = errCode_.load(std::memory_order_acquire);
    if (err != 0) return "ERR " + std::to_string(err);
    std::string n = fileName(index);
    if (n.empty()) return "-";
    if (endsWithSav(n)) n.resize(n.size() - 4);
    if (n.size() > 8)
    {
        size_t cut = 8;   // do not cut a UTF-8 sequence in half
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
        n.resize(cut);
    }
    return n.empty() ? "-" : n;
}

ImportReport ImportWorker::lastReport() const
{
    std::lock_guard<std::mutex> g(mu_);
    return report_;
}

void ImportWorker::run()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        if (scanRequested_.exchange(false, std::memory_order_acq_rel)) scan();
        if (resolveRequested_.exchange(false, std::memory_order_acq_rel)) resolvePreferred();
        const int idx = importIdx_.load(std::memory_order_acquire);
        if (idx >= 0)
        {
            importFile(idx);
            importIdx_.store(-1, std::memory_order_release);   // stays set while running: a second request is ignored
        }
        // Poll: no condition variable, so requests from the audio thread never
        // touch a lock or a futex.
        for (int i = 0; i < 5 && !stop_.load(std::memory_order_acquire) && !scanRequested_.load(std::memory_order_acquire) &&
                        importIdx_.load(std::memory_order_acquire) < 0;
             ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void ImportWorker::scan()
{
    std::vector<std::string> names;
    if (DIR* d = opendir(dir_.c_str()))
    {
        while (const dirent* e = readdir(d))
        {
            if (names.size() >= kMaxScanEntries) break;
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;   // ".", "..", macOS "._x.sav" resource forks
            if (!endsWithSav(n)) continue;
            struct stat st;
            if (lstat((dir_ + "/" + n).c_str(), &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) continue;       // directories and symbolic links are skipped
            names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end(), nameLess);
    if (names.size() > static_cast<size_t>(kMaxImportFiles)) names.resize(static_cast<size_t>(kMaxImportFiles));
    files_ = names;
    {
        std::lock_guard<std::mutex> g(mu_);
        shared_ = names;
    }
    errCode_.store(0, std::memory_order_release);   // an error text lasts until the next scan
    scans_.fetch_add(1, std::memory_order_release);
    version_.fetch_add(1, std::memory_order_release);
    resolvePreferred();
}

void ImportWorker::resolvePreferred()
{
    std::string want;
    {
        std::lock_guard<std::mutex> g(mu_);
        want = preferred_;
    }
    if (want.empty()) return;
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i] == want)
        {
            suggested_.store(static_cast<int>(i), std::memory_order_release);
            break;
        }
    std::lock_guard<std::mutex> g(mu_);
    if (preferred_ == want) preferred_.clear();   // resolved once per load
}

void ImportWorker::importFile(int index)
{
    if (index >= static_cast<int>(files_.size())) return;   // nothing at that index: no change, no error
    const std::string name = files_[static_cast<size_t>(index)];
    const std::string path = dir_ + "/" + name;
    struct stat st;
    Content c;
    ImportReport rep;
    ImportResult r;
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) r = ImportResult::ReadFailed;
    else r = import_sav(path, &c, &rep);

    if (r == ImportResult::Ok)
    {
        {
            std::lock_guard<std::mutex> g(mu_);
            lastImported_ = name;
            report_ = rep;
        }
        errCode_.store(0, std::memory_order_release);
        store_.stageImport(c);
        imports_.fetch_add(1, std::memory_order_release);
    }
    else
    {
        errCode_.store(static_cast<int>(r), std::memory_order_release);
        failures_.fetch_add(1, std::memory_order_release);
    }
    version_.fetch_add(1, std::memory_order_release);
}

}  // namespace mpcboy
