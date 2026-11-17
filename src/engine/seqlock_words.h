// src/engine/seqlock_words.h - a single-writer seqlock over a fixed-size byte
// block, shared by ContentStore (published content) and ImportWorker (file
// display table).
//
// The payload is stored as std::atomic<uint32_t> words accessed relaxed inside
// the seqlock protocol, so the structure is data-race-free by the C++ model
// (and quiet under ThreadSanitizer) without relying on fences TSan cannot see.
//
//   write:    odd sequence store, release fence, relaxed word stores, even
//             release store of the sequence (single writer thread only).
//   tryRead:  acquire load of the sequence (must be even), relaxed word loads,
//             acquire fence, re-check the sequence. One attempt; the caller
//             chooses the retry policy (unbounded with sched_yield for the
//             content store, bounded for the audio-thread display read).
//
// Real-time safe on both sides: no allocation, no lock.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sched.h>

namespace mpcboy {

template <size_t Bytes>
class SeqlockWords
{
public:
    static constexpr int kWords = static_cast<int>((Bytes + 3) / 4);

    SeqlockWords()
    {
        for (auto& w : words_) w.store(0, std::memory_order_relaxed);
    }

    // Single writer. Publishes `n` (<= Bytes) bytes; the rest of the block is
    // zero.
    void write(const void* src, size_t n)
    {
        uint32_t buf[kWords] = {};
        std::memcpy(buf, src, n);
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);               // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < kWords; ++i) words_[i].store(buf[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);               // even: stable
    }

    // One read attempt of the first `n` (<= Bytes) bytes. Returns false when a
    // write was in flight; `dst` is then unspecified. On success `*seqOut` (if
    // given) receives the sequence value (publish count * 2).
    bool tryRead(void* dst, size_t n, uint32_t* seqOut = nullptr) const
    {
        uint32_t buf[kWords];
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        for (int i = 0; i < kWords; ++i) buf[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        std::memcpy(dst, buf, n);
        if (seqOut) *seqOut = s1;
        return true;
    }

    // Retries until a stable copy is read (yielding between attempts). Returns
    // the publish count (even sequence value / 2).
    uint32_t read(void* dst, size_t n) const
    {
        uint32_t s = 0;
        while (!tryRead(dst, n, &s)) sched_yield();
        return s / 2;
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> words_[kWords];
};

}  // namespace mpcboy
