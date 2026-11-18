// src/engine/playback_state.h - each channel's live table playback, published by the audio
// thread for the UI (the Mac editor's channel LCDs and TABLE grid; UI redesign step 3).
//
// Written once per Engine::render() (and by Engine::reset(), and as idle by the plugin's
// early-return paths) with relaxed atomic stores; read by
// any thread with relaxed loads. Lock-free and allocation-free. Each channel is one 32-bit word,
// so a reader always sees one consistent set of fields for a channel (channels may come from
// adjacent blocks; that is fine for a display). The live wave RAM (16 bytes) is four words,
// read the same way: a reader can see words from two blocks, which a display tolerates.
#pragma once

#include <atomic>
#include <cstdint>

namespace mpcboy {

struct ChannelPlayback
{
    int row = 0;            // table playhead: the row the table applied last (0..15)
    bool running = false;   // a table is ticking on this channel
    int table = -1;         // table slot in use since the last trigger (0..15), -1 = none
    int note = -1;          // last triggered note on this channel (MIDI, channel transpose and
                            // legato glides included), -1 = none since reset
    int level = 0;          // current APU level 0..15 (envelope volume; Wave: 15 / 7 / 3 / 0);
                            // 0 while the channel is inactive
    bool active = false;    // the APU channel is on (sounding or holding a level)

    bool operator==(const ChannelPlayback& o) const
    {
        return row == o.row && running == o.running && table == o.table && note == o.note && level == o.level &&
               active == o.active;
    }
    bool operator!=(const ChannelPlayback& o) const { return !(*this == o); }
};

class PlaybackState
{
public:
    static constexpr int kChannels = 4;
    static constexpr int kWaveBytes = 16;

    // Word layout: bits 0..3 row, 4 running, 5 active, 8..15 note + 1 (0 = none),
    // 16..19 level, 24..28 table + 1 (0 = none).
    static uint32_t pack(const ChannelPlayback& p)
    {
        const uint32_t note = p.note < 0 ? 0u : static_cast<uint32_t>((p.note > 127 ? 127 : p.note) + 1);
        const uint32_t table = p.table < 0 ? 0u : static_cast<uint32_t>((p.table > 30 ? 30 : p.table) + 1);
        return (static_cast<uint32_t>(p.row) & 15u) | (p.running ? 1u << 4 : 0u) | (p.active ? 1u << 5 : 0u) |
               (note << 8) | ((static_cast<uint32_t>(p.level) & 15u) << 16) | (table << 24);
    }
    static ChannelPlayback unpack(uint32_t w)
    {
        ChannelPlayback p;
        p.row = static_cast<int>(w & 15u);
        p.running = (w >> 4) & 1u;
        p.active = (w >> 5) & 1u;
        p.note = static_cast<int>((w >> 8) & 255u) - 1;
        p.level = static_cast<int>((w >> 16) & 15u);
        p.table = static_cast<int>((w >> 24) & 31u) - 1;
        return p;
    }

    // Audio thread.
    void store(int ch, const ChannelPlayback& p) { ch_[ch & 3].store(pack(p), std::memory_order_relaxed); }
    // Marks one more published block (render, reset or an idle early return). A reader that sees
    // this counter stand still for a while knows process() is no longer running and shows idle.
    void markBlock() { blocks_.store(blocks_.load(std::memory_order_relaxed) + 1u, std::memory_order_release); }
    // Publishes the idle state: no table, no note, inactive, silent wave.
    void storeIdle()
    {
        const uint8_t zero[kWaveBytes] = {};
        for (int c = 0; c < kChannels; ++c) store(c, ChannelPlayback());
        storeWave(zero);
        markBlock();
    }
    void storeWave(const uint8_t bytes[kWaveBytes])
    {
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t w = static_cast<uint32_t>(bytes[4 * i]) << 24 | static_cast<uint32_t>(bytes[4 * i + 1]) << 16 |
                               static_cast<uint32_t>(bytes[4 * i + 2]) << 8 | static_cast<uint32_t>(bytes[4 * i + 3]);
            wave_[i].store(w, std::memory_order_relaxed);
        }
    }

    // Any thread.
    uint32_t blocks() const { return blocks_.load(std::memory_order_acquire); }
    ChannelPlayback load(int ch) const { return unpack(ch_[ch & 3].load(std::memory_order_relaxed)); }
    void loadWave(uint8_t out[kWaveBytes]) const
    {
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t w = wave_[i].load(std::memory_order_relaxed);
            out[4 * i] = static_cast<uint8_t>(w >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(w >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(w >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(w);
        }
    }

private:
    std::atomic<uint32_t> ch_[kChannels] = {};
    std::atomic<uint32_t> wave_[4] = {};
    std::atomic<uint32_t> blocks_{0};
};

// Reader-side staleness check (editor thread): the playback words are only as fresh as the audio
// thread's last published block. Feed it the block counter and a monotonic time; it reports true
// once the counter has not moved for more than kStaleMs (process() stopped, a host that no longer
// calls it): the editor then shows idle instead of the last note and playhead.
class PlaybackWatch
{
public:
    static constexpr long long kStaleMs = 250;
    bool stale(uint32_t blocks, long long nowMs)
    {
        if (!seen_ || blocks != last_)
        {
            seen_ = true;
            last_ = blocks;
            changedMs_ = nowMs;
        }
        return nowMs - changedMs_ > kStaleMs;
    }

private:
    bool seen_ = false;
    uint32_t last_ = 0;
    long long changedMs_ = 0;
};

}  // namespace mpcboy
