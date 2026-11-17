// Shared helpers for the engine tests: a rig that renders an Engine in host
// blocks, records every APU register write (stamped with the APU frame) through
// the test-only spy, and builds test-local table fixtures.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include "rackboy_apu.h"
}

#include "engine.h"
#include "rackboy_pitch.h"

namespace mpcboy_test {

struct W
{
    uint64_t apu;     // APU frame the write was applied at (frames rendered so far)
    uint16_t addr;
    uint8_t v;
};

struct Ev
{
    long long frame;      // relative to the start of the run() call
    bool tick;
    uint8_t status, d1, d2;
};

inline Ev noteOn(long long f, int n, int vel = 100) { return {f, false, 0x90, static_cast<uint8_t>(n), static_cast<uint8_t>(vel)}; }
inline Ev noteOff(long long f, int n) { return {f, false, 0x80, static_cast<uint8_t>(n), 0}; }
inline Ev bend(long long f, int v14)   // -8192..8191
{
    const int raw = v14 + 8192;
    return {f, false, 0xE0, static_cast<uint8_t>(raw & 0x7F), static_cast<uint8_t>(raw >> 7)};
}
inline Ev tickAt(long long f) { return {f, true, 0, 0, 0}; }

struct Rig
{
    std::unique_ptr<mpcboy::Engine> e;
    std::vector<W> log;
    std::vector<float> L, R;
    mpcboy::EngineSettings s;

    Rig()
    {
        e = std::make_unique<mpcboy::Engine>();
        s.tick.enabled = false;   // the rig injects ticks by hand (tickAt)
        rackboy_apu_set_write_spy(e->apu(), &Rig::spy, this);
    }
    ~Rig() { rackboy_apu_set_write_spy(e->apu(), nullptr, nullptr); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    static void spy(void* u, uint16_t a, uint8_t v)
    {
        auto* r = static_cast<Rig*>(u);
        r->log.push_back({r->e->apuFrame(), a, v});
    }

    void apply() { e->applySettings(s); }

    void only(int ch)
    {
        for (int i = 0; i < 4; ++i) s.ch[i].enabled = (i == ch);
    }

    // Render `frames` host frames in blocks of `block`, applying events (frames
    // relative to this call, ascending). Output is appended to L / R.
    void run(long long frames, std::vector<Ev> evs = {}, int block = 128)
    {
        long long pos = 0;
        size_t ei = 0;
        std::vector<mpcboy::EngineMidiEvent> midi;
        std::vector<int> ticks;
        while (pos < frames)
        {
            const int n = static_cast<int>(std::min<long long>(block, frames - pos));
            midi.clear();
            ticks.clear();
            size_t ej = ei;
            while (ej < evs.size() && evs[ej].frame < pos + n)
            {
                const Ev& ev = evs[ej++];
                const int off = static_cast<int>(ev.frame - pos);
                if (ev.tick) ticks.push_back(off);
                else
                {
                    mpcboy::EngineMidiEvent m;
                    m.offset = off; m.status = ev.status; m.data1 = ev.d1; m.data2 = ev.d2;
                    midi.push_back(m);
                }
            }
            ei = ej;
            mpcboy::EngineBlock blk;
            blk.midi = midi.empty() ? nullptr : midi.data();
            blk.numMidi = static_cast<int>(midi.size());
            blk.tickOffsets = ticks.empty() ? nullptr : ticks.data();
            blk.numTicks = static_cast<int>(ticks.size());
            const size_t old = L.size();
            L.resize(old + static_cast<size_t>(n));
            R.resize(old + static_cast<size_t>(n));
            e->render(blk, L.data() + old, R.data() + old, n);
            pos += n;
        }
    }
};

struct Row { int8_t tr; char c1; uint8_t p1; char c2; uint8_t p2; };

inline void setTable(rackboy::sav::RackboyTable& t, std::vector<Row> rows)
{
    rackboy::sav::table_reset(t);
    for (size_t i = 0; i < rows.size() && i < 16; ++i)
    {
        t.rows[i].transpose = rows[i].tr;
        t.rows[i].cmd1 = static_cast<uint8_t>(rows[i].c1);
        t.rows[i].cmd1_param = rows[i].p1;
        t.rows[i].cmd2 = static_cast<uint8_t>(rows[i].c2);
        t.rows[i].cmd2_param = rows[i].p2;
    }
    t.occupied = true;
}

inline std::vector<Row> majArp() { return {{0, 0, 0, 0, 0}, {4, 0, 0, 0, 0}, {3, 0, 0, 0, 0}, {-7, 'H', 1, 0, 0}}; }
inline std::vector<Row> minArp() { return {{0, 0, 0, 0, 0}, {3, 0, 0, 0, 0}, {4, 0, 0, 0, 0}, {-7, 'H', 1, 0, 0}}; }
inline std::vector<Row> octave() { return {{0, 0, 0, 0, 0}, {12, 0, 0, 0, 0}, {-12, 'H', 1, 0, 0}}; }

// Frequency writes of one pulse/wave channel reconstructed from the spy log
// (NRx3 followed by NRx4).
struct FreqWrite
{
    uint64_t apu;
    uint16_t freq;
    bool trigger;
    bool lengthBit;
};

inline std::vector<FreqWrite> freqWrites(const std::vector<W>& log, uint16_t lo, uint16_t hi, size_t from = 0)
{
    std::vector<FreqWrite> out;
    int curLo = -1;
    for (size_t i = from; i < log.size(); ++i)
    {
        if (log[i].addr == lo) curLo = log[i].v;
        else if (log[i].addr == hi && curLo >= 0)
            out.push_back({log[i].apu, static_cast<uint16_t>(curLo | ((log[i].v & 7) << 8)),
                           (log[i].v & 0x80) != 0, (log[i].v & 0x40) != 0});
    }
    return out;
}

inline double rmsDb(const std::vector<float>& v, size_t from, size_t to)
{
    if (to > v.size()) to = v.size();
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += static_cast<double>(v[i]) * v[i];
    const double rms = std::sqrt(acc / static_cast<double>(to - from));
    return rms <= 0.0 ? -300.0 : 20.0 * std::log10(rms);
}

}  // namespace mpcboy_test
