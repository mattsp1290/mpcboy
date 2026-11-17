// MpcBoyPlugin.cpp - WP1 placeholder. WP7 replaces this with the real plugin.
//
// SPDX-License-Identifier: MIT
//
// One parameter (Bend Range at index 0, per decision D9) and a quiet 440 Hz
// sine while a note is held, so the shared library links and makes sound.
//
// With -DMPCBOY_CPU_PROBE=ON (CMake option, debug only) the plugin is instead
// the WP3 CPU spike: it holds a note on the real Engine (4 channels, 4 tables
// ticking at 24/beat @ 120 BPM), times every process() call with
// clock_gettime(CLOCK_MONOTONIC), and shows mean/p99 in microseconds as the
// display text of the read-only parameter `dbg_us` (see below).

#include "mpc_plugin.h"

#include <cmath>

#ifdef MPCBOY_CPU_PROBE

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <time.h>
#include <vector>

#include "engine.h"

class MpcBoyPlugin : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "mpcboy";
        i.vendor = "mpcboy";
        i.uniqueId = MPC_FOURCC('M', 'b', 'G', 'b');   // must match "uid" in plugin/plugin.json
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MpcBoyPlugin() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam("bendrng", "Bend Range", 0, 24, 2, "st"));
        mpc::Param dbg = mpc::floatParam("dbg_us", "CPU us", 0.0f, 100000.0f, 0.0f, "us", 1);
        dbg.automatable = false;   // read-only: the text comes from formatValue
        dbgIndex_ = addParam(dbg);

        mpcboy::PolyphaseResampler48To441::ensureTable();   // never on the audio thread
        engine_ = std::make_unique<mpcboy::Engine>();
        auto& bank = engine_->ownBank();
        for (int t = 0; t < 4; ++t)
        {
            bank.tables[t].occupied = true;
            const int8_t steps[4] = {0, static_cast<int8_t>(3 + t), static_cast<int8_t>(4 - t), -7};
            for (int r = 0; r < 4; ++r) bank.tables[t].rows[r].transpose = steps[r];
            bank.tables[t].rows[3].cmd1 = 'H';
            bank.tables[t].rows[3].cmd1_param = 1;
        }
        mpcboy::EngineSettings s;
        for (int c = 0; c < 4; ++c) { s.ch[c].tableSlot = static_cast<int8_t>(c); s.ch[c].volume = 15; }
        engine_->applySettings(s);

        worker_ = std::thread([this] { workerLoop(); });
    }

    ~MpcBoyPlugin() override
    {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    void reset() override { resetPending_.store(true, std::memory_order_release); }

    void process(mpc::ProcessContext& ctx) override
    {
        const long long t0 = nowNs();
        if (resetPending_.exchange(false, std::memory_order_acq_rel))
        {
            engine_->reset();
            noteHeld_ = false;
        }
        mpcboy::EngineMidiEvent on;
        mpcboy::EngineBlock blk;
        if (!noteHeld_)
        {
            on.offset = 0; on.status = 0x90; on.data1 = 60; on.data2 = 100;
            blk.midi = &on;
            blk.numMidi = 1;
            noteHeld_ = true;
        }
        engine_->render(blk, ctx.outputs[0], ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0], ctx.numFrames);

        const long long dt = nowNs() - t0;
        const uint32_t i = head_.load(std::memory_order_relaxed);
        ring_[i % kRing].store(static_cast<uint32_t>(dt > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : dt), std::memory_order_relaxed);
        head_.store(i + 1, std::memory_order_release);
    }

    bool formatValue(int index, float, std::string& out) override
    {
        if (index != dbgIndex_) return false;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "m%.0f p99 %.0f",
                      static_cast<double>(meanX10_.load(std::memory_order_relaxed)) / 10.0,
                      static_cast<double>(p99X10_.load(std::memory_order_relaxed)) / 10.0);
        out = buf;
        return true;
    }

private:
    static constexpr uint32_t kRing = 32768;          // ~95 s of 128-frame blocks

    static long long nowNs()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }

    // Debug-only worker: once per second, mean/p99 over the last <= 60 s of
    // blocks. Calls requestDisplayUpdate() from here, never from process().
    void workerLoop()
    {
        std::vector<uint32_t> snap;
        snap.reserve(kRing);
        while (!stop_.load(std::memory_order_acquire))
        {
            for (int i = 0; i < 20 && !stop_.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint32_t head = head_.load(std::memory_order_acquire);
            const uint32_t n = std::min<uint32_t>(head, 20000);   // ~58 s at 128 frames
            if (n < 16) continue;
            snap.clear();
            double sum = 0.0;
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint32_t v = ring_[(head - 1 - k) % kRing].load(std::memory_order_relaxed);
                snap.push_back(v);
                sum += v;
            }
            std::sort(snap.begin(), snap.end());
            const double p99 = snap[static_cast<size_t>(0.99 * static_cast<double>(n - 1))];
            meanX10_.store(static_cast<uint32_t>(sum / n / 100.0), std::memory_order_relaxed);   // ns -> us * 10
            p99X10_.store(static_cast<uint32_t>(p99 / 100.0), std::memory_order_relaxed);
            requestDisplayUpdate();
        }
    }

    std::unique_ptr<mpcboy::Engine> engine_;
    int dbgIndex_ = -1;
    std::atomic<bool> resetPending_{true};
    bool noteHeld_ = false;

    std::atomic<uint32_t> ring_[kRing];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> meanX10_{0};
    std::atomic<uint32_t> p99X10_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

#else   // ---------------------------------------------------------------- placeholder

class MpcBoyPlugin : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "mpcboy";
        i.vendor = "mpcboy";
        i.uniqueId = MPC_FOURCC('M', 'b', 'G', 'b');   // must match "uid" in plugin/plugin.json
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MpcBoyPlugin() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam("bendrng", "Bend Range", 0, 24, 2, "st"));
    }

    void reset() override
    {
        phase_ = 0.0;
        held_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            if (ev.isNoteOn()) { if (held_ == 0) phase_ = 0.0; ++held_; }
            else if (ev.isNoteOff()) { if (held_ > 0) --held_; }
            else if (ev.isAllNotesOff()) held_ = 0;
        }
        render(ctx, pos, ctx.numFrames);
    }

private:
    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        if (held_ == 0) return;
        const double inc = 440.0 / ctx.sampleRate;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = from; i < to; ++i)
        {
            const float s = 0.1f * (float)std::sin(6.283185307179586 * phase_);
            L[i] = s;
            if (R) R[i] = s;
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }

    double phase_ = 0.0;
    int held_ = 0;
};

#endif

MPC_PLUGIN_ENTRY(MpcBoyPlugin)
