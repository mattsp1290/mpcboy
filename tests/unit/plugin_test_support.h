// Shared plugin rig for tests that drive the real MpcBoyPlugin with synthetic
// ProcessContexts (plugin_test.cpp, import_plugin_test.cpp). The thread that
// calls process() is "the audio thread" of that plugin's store (the mark is per
// instance, so it dies with the rig).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doctest/doctest.h"
#include "MpcBoyPlugin.h"
#include "content_store.h"

namespace mpcboy_test {

template <typename Fn>
bool waitFor(Fn&& fn, int timeoutMs = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

// Fake time source for the trigger debounce: tests advance it explicitly instead of sleeping.
inline long long g_fakeNs = 1'000'000'000LL;
inline long long fakeNow() { return g_fakeNs; }
inline void advanceMs(int ms) { g_fakeNs += static_cast<long long>(ms) * 1'000'000LL; }

// The file name behind the newest content (what a project save would store).
inline std::string liveName(MpcBoyPlugin& p)
{
    mpcboy::Content c;
    mpcboy::ContentSource src;
    p.content().readLatest(c, &src);
    return std::string(src.name);
}

struct PluginRig
{
    std::unique_ptr<MpcBoyPlugin> p;
    std::vector<float> L, R;
    long long pos = 0;
    double bpm = 120.0;
    bool tempoValid = true, playing = false, ppqValid = false;
    double ppqStart = 0.0;

    // `clock` (optional) replaces the import-trigger debounce time source.
    explicit PluginRig(MpcBoyPlugin::NowFn clock = nullptr)
    {
        p = std::make_unique<MpcBoyPlugin>();
        if (clock) p->setClockForTest(clock);
        p->prepare(44100.0, 512);
    }

    // One process() call with `n` frames and the given block-relative events.
    void block(int n = 128, const std::vector<mpc::MidiEvent>& midi = {})
    {
        const size_t old = L.size();
        L.resize(old + static_cast<size_t>(n));
        R.resize(old + static_cast<size_t>(n));
        float* outs[2] = {L.data() + old, R.data() + old};
        mpc::ProcessContext ctx;
        ctx.outputs = outs;
        ctx.numOutputs = 2;
        ctx.numFrames = n;
        ctx.midi = midi.empty() ? nullptr : midi.data();
        ctx.numMidiEvents = static_cast<int>(midi.size());
        ctx.sampleRate = 44100.0;
        ctx.transport.valid = true;
        ctx.transport.bpm = bpm;
        ctx.transport.tempoValid = tempoValid;
        ctx.transport.playing = playing;
        ctx.transport.ppqValid = ppqValid;
        ctx.transport.ppqPos = ppqStart + static_cast<double>(pos) / 44100.0 * bpm / 60.0;
        p->process(ctx);
        pos += n;
    }

    // Run to absolute frame `end` in blocks of `blk`, with absolute-frame MIDI events.
    void run(long long end, std::vector<mpc::MidiEvent> evs, int blk)
    {
        size_t ei = 0;
        std::vector<mpc::MidiEvent> m;
        while (pos < end)
        {
            const int n = static_cast<int>(std::min<long long>(blk, end - pos));
            m.clear();
            while (ei < evs.size() && evs[ei].sampleOffset < pos + n)
            {
                mpc::MidiEvent e = evs[ei++];
                e.sampleOffset = static_cast<int>(e.sampleOffset - pos);
                m.push_back(e);
            }
            block(n, m);
        }
    }

    std::vector<uint8_t> blob() const
    {
        std::vector<uint8_t> v;
        p->saveExtraState(v);
        return v;
    }

    // Rescan the import folder and wait until the worker has listed the files.
    void rescan()
    {
        const uint32_t before = p->importer().scansDone();
        p->importer().requestScan();
        REQUIRE(waitFor([&] { return p->importer().scansDone() > before; }));
    }
};

}  // namespace mpcboy_test
