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
// Display text for Imp File is published by the worker into a fixed-size table
// behind a seqlock (see displayTextInto), so the host's parameter-display calls
// (which can be triggered by the plugin's own audioMasterUpdateDisplay from
// process()) never lock or allocate here.
//
// No SDK dependency: the directory is passed in (the plugin passes
// mpc::contentPath("Import")).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
    // The file name of the import behind the newest content (saved in the
    // project): follows what the audio thread actually made live, so a project
    // load that replaces a pending import, or an Imp Undo, does not leave a name
    // for content that is not live (see ContentStore::importTicket). Without an
    // import in the newest content it is the name set by setLastImportedName (the
    // one stored in the loaded project). A name a plugin
    // would like selected after the next scan (set after a state load).
    std::string lastImportedName() const;
    void setLastImportedName(const std::string& name);
    void setPreferredName(const std::string& name);

    int fileCount() const;
    std::string fileName(int index) const;   // "" when out of range
    // Text for Imp File: the file name without ".sav", at most 8 bytes; "-" when
    // the list is empty or `index` is out of range; "ERR <code>" after a failed
    // import until the next scan finishes.
    // Allocating convenience wrapper over displayTextInto (tests, UI thread).
    std::string displayText(int index) const;
    // Real-time safe (any thread, including the audio thread): no mutex, no
    // allocation. Writes at most kDisplayBufSize - 1 characters plus a NUL into
    // `buf`, which must hold kDisplayBufSize bytes, and returns the length. Reads
    // the published table with a bounded seqlock retry; if a scan is publishing
    // for longer than that it returns "-" (the next refresh shows the real text).
    static constexpr int kDisplayBufSize = 16;
    size_t displayTextInto(int index, char* buf) const;

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

    // Published display names: kDispWords 32-bit words per entry (8 characters
    // plus NUL, zero padded), one writer (the worker, in scan()), any number of
    // readers. Words are relaxed atomics inside the seqlock protocol, which
    // keeps it free of data races for TSan.
    static constexpr int kDispWords = 3;
    void publishDisplay(const std::vector<std::string>& names);
    std::atomic<uint32_t> dispSeq_{0};
    std::atomic<uint32_t> dispCount_{0};
    std::atomic<uint32_t> dispWords_[kMaxImportFiles * kDispWords];

    mutable std::mutex mu_;   // guards the members below
    std::vector<std::string> shared_;
    std::string lastImported_;                              // from the loaded project
    std::vector<std::pair<uint32_t, std::string>> imports_by_ticket_;   // recent imports, oldest first
    std::string preferred_;
    ImportReport report_;

    std::thread thread_;
};

}  // namespace mpcboy
