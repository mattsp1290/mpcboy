#include "content_store.h"

#include <cstring>
#include <utility>
#include <sched.h>

#include "bank_adapter.h"

namespace mpcboy {

namespace {
thread_local bool tlAudioThread = false;
}

ContentStore::ContentStore()
{
    clearContent(live_);
    clearContent(staged_);
    clearContent(undo_);
    for (auto& w : words_) w.store(0, std::memory_order_relaxed);
    initBank(bank_, live_);
    publish();
}

void ContentStore::markAudioThread(bool on) { tlAudioThread = on; }
bool ContentStore::onAudioThread() { return tlAudioThread; }

void ContentStore::touchLive() { ++liveGen_; }

void ContentStore::setLive(const Content& c)
{
    live_ = c;
    syncBank(bank_, live_);
    touchLive();
    publish();
}

void ContentStore::publish()
{
    uint32_t buf[kWords] = {};
    std::memcpy(buf, &live_, sizeof(Content));
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);               // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < kWords; ++i) words_[i].store(buf[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);               // even: stable
    publishedGen_ = liveGen_;
}

bool ContentStore::beginBlock()
{
    bool changed = false;
    int expected = Ready;
    if (stagedState_.compare_exchange_strong(expected, Reading, std::memory_order_acquire))
    {
        if (stagedIsImport_)
        {
            undo_ = live_;   // one level of undo: the content just before this import
            undoValid_.store(true, std::memory_order_release);
        }
        live_ = staged_;
        syncBank(bank_, live_);
        touchLive();
        ++bulkGen_;
        publish();
        // Release the slot only after publishing, so readLatest() never sees an
        // Empty slot while the published snapshot still holds pre-load content.
        stagedState_.store(Empty, std::memory_order_release);
        changed = true;
    }
    if (undoRequested_.exchange(false, std::memory_order_acq_rel) && undoValid_.load(std::memory_order_acquire))
    {
        std::swap(live_, undo_);   // a second undo re-applies the import
        syncBank(bank_, live_);
        touchLive();
        ++bulkGen_;
        publish();
        changed = true;
    }
    if (changed) return true;
    if (publishedGen_ != liveGen_)
    {
        syncBank(bank_, live_);   // edits touch live only
        publish();
        changed = true;
    }
    return changed;
}

void ContentStore::stage(const Content& c) { stageImpl(c, false); }

void ContentStore::stageImpl(const Content& c, bool isImport)
{
    for (;;)
    {
        int expected = Empty;
        if (stagedState_.compare_exchange_weak(expected, Writing, std::memory_order_acquire)) break;
        if (expected == Ready)
        {
            // Replace a load the audio thread has not consumed yet.
            if (stagedState_.compare_exchange_weak(expected, Writing, std::memory_order_acquire)) break;
            continue;
        }
        sched_yield();   // Writing (another writer) or Reading (one memcpy)
    }
    staged_ = c;
    stagedIsImport_ = isImport;
    stagedState_.store(Ready, std::memory_order_release);
}

void ContentStore::readLatest(Content& out)
{
    for (;;)
    {
        int state = stagedState_.load(std::memory_order_acquire);
        if (state == Empty)
        {
            readPublished(out);
            return;
        }
        if (state == Ready)
        {
            if (!stagedState_.compare_exchange_weak(state, Reading, std::memory_order_acquire)) continue;
            out = staged_;
            stagedState_.store(Ready, std::memory_order_release);
            return;
        }
        sched_yield();   // Writing (a load in flight) or Reading (one memcpy)
    }
}

uint32_t ContentStore::readPublished(Content& out) const
{
    uint32_t buf[kWords];
    for (;;)
    {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) { sched_yield(); continue; }
        for (int i = 0; i < kWords; ++i) buf[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == s1)
        {
            std::memcpy(&out, buf, sizeof(Content));
            return s1 / 2;
        }
    }
}

}  // namespace mpcboy
