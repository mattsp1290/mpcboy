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
// Other threads:      stage, stageImport, requestUndo, readPublished, readLatest
//
// Every content slot (live, staged, undo, published) carries the name of the
// file it came from (ContentSource: the .sav import's file name, or the name
// stored in a loaded project; "" = none), so the name follows the content
// through staging, undo and publishing with no separate bookkeeping.
//
// WP11 undo: content staged with stageImport() is tagged as an import. When the
// audio thread consumes it, it first copies `live` into a preallocated `undo`
// Content (about 2 KB memcpy) and marks it valid; requestUndo() makes the audio
// thread swap `undo` and `live` at the next beginBlock() (so a second undo
// re-applies the import). All undo work stays on the audio thread. The undo
// buffer is never saved.
// WP7 wiring: loadExtraState -> stage(); saveExtraState / formatValue ->
// readPublished() (the plugin's formatValue does not read the store; the import
// file list has its own seqlock table in ImportWorker); the plugin's reset() only
// sets resetPending_ and process() calls beginBlock() once (it is the single
// owner), then Engine::reset() and Engine::render().
//
// The published snapshot sits behind SeqlockWords (seqlock_words.h).
#pragma once

#include <atomic>
#include <cstdint>

#include "rackboy_sav_types.h"
#include "seqlock_words.h"
#include "table_bank.h"

namespace mpcboy {

// Where a content snapshot came from: a file name, NUL-terminated, "" = none.
// kMaxSourceName equals the state format's name limit (kStateMaxName).
constexpr size_t kMaxSourceName = 255;
struct ContentSource
{
    char name[kMaxSourceName + 1] = {};

    void set(const char* n)
    {
        size_t i = 0;
        if (n)
            for (; n[i] != '\0' && i < kMaxSourceName; ++i) name[i] = n[i];
        name[i] = '\0';
    }
};

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
    // `sourceName` (copied; nullptr = none) is the name of the file the content
    // came from (the name stored in a loaded project).
    void stage(const Content& c, const char* sourceName = nullptr);

    // Like stage(), but tagged as a .sav import: the audio thread keeps a copy
    // of the previous live content (and its source) for one level of undo.
    void stageImport(const Content& c, const char* sourceName = nullptr);

    // Asks the audio thread to swap the undo copy and live at the next
    // beginBlock(). Ignored there when no undo copy is valid.
    void requestUndo() { undoRequested_.store(true, std::memory_order_release); }
    bool undoAvailable() const { return undoValid_.load(std::memory_order_acquire); }

    // Copies the published snapshot (and optionally its source) into `out`,
    // retrying while a publish is in flight. Returns the publish count.
    uint32_t readPublished(Content& out, ContentSource* src = nullptr) const;

    // Copies the newest content: a staged load the audio thread has not
    // consumed yet, otherwise the published snapshot. Used by state save, so a
    // save between a load and the next audio block still returns the load (with
    // the name that belongs to it).
    void readLatest(Content& out, ContentSource* src = nullptr);

    // Edits attempted off the audio thread (debug counter).
    uint32_t rejectedEdits() const { return rejectedEdits_.load(std::memory_order_relaxed); }

private:
    enum StagedState : int { Empty = 0, Writing = 1, Ready = 2, Reading = 3 };
    struct Published
    {
        Content c;
        ContentSource src;
    };

    void touchLive();
    void publish();
    void stageImpl(const Content& c, const char* sourceName, bool isImport);
    // Shared tail of a consumed staged load / undo: rebuild the bank view and
    // republish.
    void adoptLive();

    Content live_;
    ContentSource liveSrc_;
    rackboy::sav::RackboyBank bank_;
    uint32_t liveGen_ = 0;
    uint32_t publishedGen_ = ~0u;

    Content staged_;
    ContentSource stagedSrc_;
    bool stagedImport_ = false;     // written in Writing, read in Reading
    std::atomic<int> stagedState_{Empty};

    std::atomic<uintptr_t> audioThread_{0};   // thread id of the marked audio thread, 0 = none

    Content undo_;                  // audio thread only
    ContentSource undoSrc_;         // audio thread only
    std::atomic<bool> undoValid_{false};
    std::atomic<bool> undoRequested_{false};
    uint32_t bulkGen_ = 0;

    SeqlockWords<sizeof(Published)> published_;

    std::atomic<uint32_t> rejectedEdits_{0};
};

}  // namespace mpcboy
