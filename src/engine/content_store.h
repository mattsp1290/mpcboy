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
// Other threads:      stage, readPublished
// WP7 wiring: loadExtraState -> stage(); saveExtraState / formatValue ->
// readPublished(); the plugin's reset() only sets resetPending_ and process()
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
    // Marks / unmarks the calling thread as the audio thread (a thread_local
    // flag; the plugin sets it at the top of every process()). Never cleared by
    // the store itself.
    static void markAudioThread(bool on = true);
    static bool onAudioThread();

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

    // Copies the published snapshot into `out`, retrying while a publish is in
    // flight. Returns the publish count (even seqlock value / 2).
    uint32_t readPublished(Content& out) const;

    // Edits attempted off the audio thread (debug counter).
    uint32_t rejectedEdits() const { return rejectedEdits_.load(std::memory_order_relaxed); }

private:
    enum StagedState : int { Empty = 0, Writing = 1, Ready = 2, Reading = 3 };
    static constexpr int kWords = static_cast<int>((sizeof(Content) + 3) / 4);

    void touchLive();
    void publish();

    Content live_;
    rackboy::sav::RackboyBank bank_;
    uint32_t liveGen_ = 0;
    uint32_t publishedGen_ = ~0u;

    Content staged_;
    std::atomic<int> stagedState_{Empty};

    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords];

    std::atomic<uint32_t> rejectedEdits_{0};
};

}  // namespace mpcboy
