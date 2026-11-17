// src/plugin/import_worker.h - the .sav import worker (WP11, 06-lsdj-sav-import.md).
//
// One thread per plugin instance, started in the constructor and joined in the
// destructor. It scans <dir> (Content/Import: depth 1, regular `.sav` files
// only, symbolic links not followed, dot-files skipped, sorted by name, at most
// 64 entries) at start and on request, and imports one file on request. A
// successful import goes through ContentStore::stageImport (so the audio thread
// keeps one level of undo). No file I/O ever happens on the audio thread: the
// request functions below are lock-free and only set atomics, and the worker
// polls them every 10 ms.
//
// No SDK dependency: the directory is passed in (the plugin passes
// mpc::contentPath("Import")).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "content_store.h"
#include "sav_import.h"

namespace mpcboy {

constexpr int kMaxImportFiles = 64;

class ImportWorker
{
public:
    ImportWorker(std::string dir, ContentStore& store);
    ~ImportWorker();
    ImportWorker(const ImportWorker&) = delete;
    ImportWorker& operator=(const ImportWorker&) = delete;

    // ---- requests: any thread (including the audio thread), lock-free --------
    void requestScan() { scanRequested_.store(true, std::memory_order_release); }
    // Imports the file at `index` (0-based, into the last scan's list). Returns
    // false when an import is already pending or running (the request is
    // ignored).
    bool requestImport(int index);

    // ---- state shared with the plugin (UI / any non-audio thread; short mutex) -
    // The file name last imported (saved in the project) and a name the plugin
    // would like selected after the next scan (set after a state load).
    std::string lastImportedName() const;
    void setLastImportedName(const std::string& name);
    void setPreferredName(const std::string& name);

    int fileCount() const;
    std::string fileName(int index) const;   // "" when out of range
    // Text for Imp File: the file name without ".sav", at most 8 bytes; "-" when
    // the list is empty or `index` is out of range; "ERR <code>" after a failed
    // import until the next scan finishes.
    std::string displayText(int index) const;

    // ---- audio-thread safe (atomics) ------------------------------------------
    // Bumped whenever displayText() may have changed (a finished scan, an import
    // result).
    uint32_t version() const { return version_.load(std::memory_order_acquire); }
    // Index (0-based) the plugin should select, or -1; consumed by the call.
    int takeSuggestedIndex() { return suggested_.exchange(-1, std::memory_order_acq_rel); }
    bool busy() const { return importIdx_.load(std::memory_order_acquire) >= 0; }

    // ---- counters (tests / diagnostics) ---------------------------------------
    uint32_t scansDone() const { return scans_.load(std::memory_order_acquire); }
    uint32_t importsDone() const { return imports_.load(std::memory_order_acquire); }      // successful
    uint32_t importsFailed() const { return failures_.load(std::memory_order_acquire); }
    int lastErrorCode() const { return errCode_.load(std::memory_order_acquire); }
    ImportReport lastReport() const;

private:
    void run();
    void scan();
    void importFile(int index);
    void resolvePreferred();

    const std::string dir_;
    ContentStore& store_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> scanRequested_{false};
    std::atomic<bool> resolveRequested_{false};
    std::atomic<int> importIdx_{-1};   // pending or running import, -1 = idle
    std::atomic<uint32_t> version_{0};
    std::atomic<int> suggested_{-1};
    std::atomic<int> errCode_{0};
    std::atomic<uint32_t> scans_{0};
    std::atomic<uint32_t> imports_{0};
    std::atomic<uint32_t> failures_{0};

    // Worker-thread private.
    std::vector<std::string> files_;

    mutable std::mutex mu_;   // guards the members below
    std::vector<std::string> shared_;
    std::string lastImported_;
    std::string preferred_;
    ImportReport report_;

    std::thread thread_;
};

}  // namespace mpcboy
