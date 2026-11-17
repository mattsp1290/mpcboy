#include "content_store.h"

#include <cstring>
#include <utility>
#include <pthread.h>
#include <sched.h>
#include <type_traits>

#include "bank_adapter.h"

namespace mpcboy {

namespace {
// pthread_self() reads the thread pointer register: no TLS block lookup, no allocation.
template <typename T>
uintptr_t toId(T t, std::true_type) { return reinterpret_cast<uintptr_t>(t); }
template <typename T>
uintptr_t toId(T t, std::false_type) { return static_cast<uintptr_t>(t); }

uintptr_t selfId() { return toId(pthread_self(), std::is_pointer<pthread_t>()); }
}  // namespace

ContentStore::ContentStore()
{
    clearContent(live_);
    clearContent(staged_);
    clearContent(undo_);
    for (auto& w : words_) w.store(0, std::memory_order_relaxed);
    initBank(bank_, live_);
    publish();
}

void ContentStore::markAudioThread(bool on)
{
    const uintptr_t me = selfId();
    if (on)
    {
        audioThread_.store(me, std::memory_order_relaxed);
    }
    else
    {
        uintptr_t expected = me;
        audioThread_.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    }
}

bool ContentStore::onAudioThread() const
{
    const uintptr_t a = audioThread_.load(std::memory_order_relaxed);
    return a != 0 && a == selfId();
}

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
        if (stagedTicket_ != 0)
        {
            undo_ = live_;   // one level of undo: the content just before this import
            undoTicket_ = curTicket_.load(std::memory_order_relaxed);
            curTicket_.store(stagedTicket_, std::memory_order_release);
            undoValid_.store(true, std::memory_order_release);
        }
        else
        {
            // A project / preset load starts a new history: an undo copy from an
            // earlier import must not bring another project's tables back.
            undoValid_.store(false, std::memory_order_release);
            undoRequested_.store(false, std::memory_order_relaxed);
            undoTicket_ = 0;
            curTicket_.store(0, std::memory_order_release);
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
        const uint32_t was = curTicket_.load(std::memory_order_relaxed);
        curTicket_.store(undoTicket_, std::memory_order_release);
        undoTicket_ = was;
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

void ContentStore::stage(const Content& c) { stageImpl(c, 0); }

void ContentStore::stageImpl(const Content& c, uint32_t importTicket)
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
    stagedTicket_ = importTicket;
    stagedState_.store(Ready, std::memory_order_release);
}

uint32_t ContentStore::importTicket()
{
    for (;;)
    {
        int state = stagedState_.load(std::memory_order_acquire);
        if (state == Empty) return curTicket_.load(std::memory_order_acquire);
        if (state == Ready)
        {
            if (!stagedState_.compare_exchange_weak(state, Reading, std::memory_order_acquire)) continue;
            const uint32_t t = stagedTicket_;
            stagedState_.store(Ready, std::memory_order_release);
            return t;
        }
        sched_yield();   // Writing or Reading
    }
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
