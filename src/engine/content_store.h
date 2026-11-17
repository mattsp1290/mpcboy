// src/engine/content_store.h - thread handoff for the tables and the wave bank
// (WP5, 03-tables-and-tempo-sync.md "Concurrency and lifecycle").
//
//   live       audio thread only (the Engine reads it through bank()).
//   staged     one slot for bulk content (state load, .sav import) written by
//              any other thread: Empty -> Writing -> Ready -> Reading -> Empty.
//              A newer load replaces one the audio thread has not consumed.
//   published  a copy of live behind a seqlock for readers on other threads
//              (saveExtraState, formatValue). Written by the audio thread only.
//
// Audio thread API:   markAudioThread, beginBlock, editLive, live, bank
//
// "The audio thread" is whichever thread last called markAudioThread() on this
// store (the plugin does it at the top of every process()). The mark is sticky:
// it is never cleared by the store, because screen edits arrive between
// process() calls. A host that restores a chunk on the same thread that runs
// process() would therefore be treated as the audio thread; the device
// contract says loads come from the UI thread (G-EDIT row in
// docs/DEVICE_TESTS.md confirms it). The mark is per store (per plugin
// instance) and uses no thread_local storage: dynamic TLS in a dlopen'd module
// can allocate or take a loader lock on first access.
// Other threads:      stage, stageImport, importTicket, requestUndo, readPublished
//
// WP11 undo: content staged with stageImport() is tagged as an import. When the
// audio thread consumes it, it first copies `live` into a preallocated `undo`
// Content (about 2 KB memcpy) and marks it valid; requestUndo() makes the audio
// thread swap `undo` and `live` at the next beginBlock() (so a second undo
// re-applies the import). All undo work stays on the audio thread. The undo
// buffer is never saved.
// WP7 wiring: loadExtraState -> stage(); saveExtraState / formatValue ->
// readPublished() (the plugin's formatValue does not read the store; the import
// file list has its own seqlock table in ImportWorker); the plugin's reset() only sets resetPending_ and process()
// runs Engine::reset() then Engine::render(), whose first step is beginBlock().
//
// The published words are std::atomic<uint32_t> accessed relaxed inside the
// seqlock protocol, so the structure is data-race-free by the C++ model (and
// quiet under ThreadSanitizer) without relying on fences TSan cannot see.
#pragma once

#include <atomic>
#include <cstdint>

#include "rackboy_sav_types.h"
#include "table_bank.h"

namespace mpcboy {

class ContentStore
{
public:
    // Large object (embeds a RackboyBank view): create it on the heap.
    ContentStore();

    // ---- audio thread -------------------------------------------------------
    // Marks / unmarks the calling thread as this store's audio thread (an
    // atomic thread id; the plugin sets it at the top of every process()).
    // `false` clears the mark only when the caller is the marked thread. Never
    // cleared by the store itself. Lock-free and allocation-free.
    void markAudioThread(bool on = true);
    bool onAudioThread() const;

    // Start of a block: consumes a Ready staged slot into live (rebuilding the
    // bank view) and republishes if live changed since the last publish.
    // Returns true when live changed. Call only on the audio thread.
    bool beginBlock();

    // Applies `fn(Content&)` to live when called on the audio thread; from any
    // other thread it does nothing and counts a rejected edit. The change is
    // published by the next beginBlock(). Returns whether it was applied.
    template <typename Fn>
    bool editLive(Fn&& fn)
    {
        if (!onAudioThread())
        {
            rejectedEdits_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        fn(live_);
        touchLive();
        return true;
    }

    // Incremented on the audio thread whenever bulk content replaced live (a
    // consumed staged load or import, or an undo): the plugin resyncs the
    // editor cursor when it changes.
    uint32_t bulkGeneration() const { return bulkGen_; }

    const Content& live() const { return live_; }
    // Table / wave view for table_tick; valid for the lifetime of the store.
    const rackboy::sav::RackboyBank& bank() const { return bank_; }
    // Incremented whenever live changes (edit, consumed staged content, setLive).
    uint32_t liveGeneration() const { return liveGen_; }

    // Constructor-time load (before any other thread exists): replaces live
    // and publishes immediately.
    void setLive(const Content& c);

    // ---- any other thread ---------------------------------------------------
    // Hands bulk content to the audio thread. Never waits for the audio thread
    // except for the one-memcpy Reading window; a newer call replaces a Ready
    // slot the audio thread has not consumed yet.
    void stage(const Content& c);

    // Like stage(), but tagged as a .sav import: the audio thread keeps a copy
    // of the previous live content for one level of undo.
    // Returns the import's ticket (never 0), which importTicket() reports while
    // that content is the newest (staged or live).
    uint32_t stageImport(const Content& c)
    {
        const uint32_t t = nextImportTicket();
        stageImpl(c, t);
        return t;
    }
    // Two-step form for a caller that must record the ticket before the content
    // becomes visible (ImportWorker registers the file name under it).
    uint32_t nextImportTicket() { return nextTicket_.fetch_add(1, std::memory_order_relaxed); }
    void stageImportWithTicket(const Content& c, uint32_t ticket) { stageImpl(c, ticket); }

    // Ticket of the import the newest content came from: the staged slot's ticket
    // if one is waiting, otherwise the live content's. 0 = the content did not come
    // from an import (a project load, or the start). An undo swaps the live ticket
    // with the undo copy's, so the answer follows what is actually live. Any thread
    // (never waits for the audio thread beyond the one-memcpy Reading window).
    uint32_t importTicket();
    // Asks the audio thread to swap the undo copy and live at the next
    // beginBlock(). Ignored there when no undo copy is valid.
    void requestUndo() { undoRequested_.store(true, std::memory_order_release); }
    bool undoAvailable() const { return undoValid_.load(std::memory_order_acquire); }

    // Copies the published snapshot into `out`, retrying while a publish is in
    // flight. Returns the publish count (even seqlock value / 2).
    uint32_t readPublished(Content& out) const;

    // Copies the newest content: a staged load the audio thread has not
    // consumed yet, otherwise the published snapshot. Used by state save, so a
    // save between a load and the next audio block still returns the load.
    void readLatest(Content& out);

    // Edits attempted off the audio thread (debug counter).
    uint32_t rejectedEdits() const { return rejectedEdits_.load(std::memory_order_relaxed); }

private:
    enum StagedState : int { Empty = 0, Writing = 1, Ready = 2, Reading = 3 };
    static constexpr int kWords = static_cast<int>((sizeof(Content) + 3) / 4);

    void touchLive();
    void publish();
    void stageImpl(const Content& c, uint32_t importTicket);

    Content live_;
    rackboy::sav::RackboyBank bank_;
    uint32_t liveGen_ = 0;
    uint32_t publishedGen_ = ~0u;

    Content staged_;
    uint32_t stagedTicket_ = 0;     // 0 = plain load, else the import's ticket (written in Writing, read in Reading)
    std::atomic<int> stagedState_{Empty};

    std::atomic<uintptr_t> audioThread_{0};   // thread id of the marked audio thread, 0 = none

    Content undo_;                  // audio thread only
    std::atomic<uint32_t> nextTicket_{1};
    std::atomic<uint32_t> curTicket_{0};   // import ticket of live (audio thread writes)
    uint32_t undoTicket_ = 0;               // ticket of the undo copy (audio thread only)
    std::atomic<bool> undoValid_{false};
    std::atomic<bool> undoRequested_{false};
    uint32_t bulkGen_ = 0;

    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords];

    std::atomic<uint32_t> rejectedEdits_{0};
};

}  // namespace mpcboy
