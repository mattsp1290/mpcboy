// MpcBoyPlugin.cpp - the mpcboy instrument (WP7).
//
// SPDX-License-Identifier: MIT
//
// process(): reset -> staged content -> editor cursor resync -> settings
// snapshot -> Engine::render -> master volume (10 ms smoother) -> host refresh.
//
// With -DMPCBOY_CPU_PROBE=ON (debug only) the same plugin additionally holds a
// Stack4 note by itself, times every process() call and shows mean/p99 in
// microseconds as the display text of the read-only parameter `dbg_us`
// (appended last). See docs/BENCH.md.

#include "MpcBoyPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef MPCBOY_CPU_PROBE
#include <chrono>
#include <time.h>
#endif

#include "factory_content.h"
#include "state_codec.h"

using namespace mpcboy;

namespace {

#ifdef MPCBOY_CPU_PROBE
long long nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
#endif

// 10 ms one-pole smoother at 44.1 kHz.
const float kGainCoef = 1.0f - std::exp(-1.0f / (0.010f * 44100.0f));

}  // namespace

mpc::PluginInfo MpcBoyPlugin::makeInfo()
{
    mpc::PluginInfo i;
    i.name = "mpcboy";
    i.vendor = "mpcboy";
    i.uniqueId = MPC_FOURCC('M', 'b', 'G', 'b');   // must match "uid" in plugin/plugin.json
    i.version = 100;                               // 0.1.0.0
    i.isInstrument = true;
    i.numInputs = 0;
    i.numOutputs = 2;
    i.latencySamples = 0;                          // the ~118-frame resampler delay is documented, not reported (R7)
    return i;
}

MpcBoyPlugin::MpcBoyPlugin() : mpc::Plugin(makeInfo())
{
    // All allocation happens here, never on the audio thread.
    PolyphaseResampler48To441::ensureTable();
    engine_ = std::make_unique<Engine>();
    store_ = std::make_unique<ContentStore>();
    {
        Content factory;
        loadFactoryContent(factory);
        store_->setLive(factory);   // before any other thread exists
    }
    engine_->attachContent(store_.get());

    for (const mpc::Param& p : buildParams()) addParam(p);
    for (const ProgramDef& d : programDefs()) addProgram(d.name, d.values);

    float plain[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i) plain[i] = get(i);
    engine_->applySettings(proposedSettings(plain));
    gain_ = volumeToGain(get(P_Volume));

#ifdef MPCBOY_CPU_PROBE
    setProgram(kProgramStack4);
    for (auto& r : ring_) r.store(0, std::memory_order_relaxed);
    worker_ = std::thread([this] { workerLoop(); });
#endif
}

MpcBoyPlugin::~MpcBoyPlugin()
{
#ifdef MPCBOY_CPU_PROBE
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
#endif
}

void MpcBoyPlugin::prepare(double sampleRate, int)
{
    // Never reallocates. Anything but 44.1 kHz is unsupported: stay silent.
    silent_.store(std::fabs(sampleRate - 44100.0) > 0.5, std::memory_order_release);
}

void MpcBoyPlugin::reset() { resetPending_.store(true, std::memory_order_release); }

void MpcBoyPlugin::parameterChanged(int index, float plainValue)
{
    if (index == ED_Table || index == ED_Row)
    {
        editor_.markCursorDirty();
    }
    else if (index >= ED_Vol && index <= ED_Val2)
    {
        editor_.onCellParam(index - ED_Vol, static_cast<int>(std::lround(plainValue)),
                            getInt(ED_Table) - 1, getInt(ED_Row), *store_);
    }
}

void MpcBoyPlugin::saveExtraState(std::vector<uint8_t>& out)
{
    Content c;
    store_->readLatest(c);
    encodeState(c, out);
}

void MpcBoyPlugin::loadExtraState(const uint8_t* data, size_t size)
{
    Content c;
    if (!decodeState(data, size, c)) return;   // corrupt or foreign: keep the current content
    store_->stage(c);
}

bool MpcBoyPlugin::formatValue(int index, float plainValue, std::string& out)
{
#ifdef MPCBOY_CPU_PROBE
    if (index == P_DbgUs)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "m%.0f p99 %.0f",
                      static_cast<double>(meanX10_.load(std::memory_order_relaxed)) / 10.0,
                      static_cast<double>(p99X10_.load(std::memory_order_relaxed)) / 10.0);
        out = buf;
        return true;
    }
#endif
    return formatParamValue(index, plainValue, out);
}

void MpcBoyPlugin::snapshotSettings(bool force)
{
    bool changed = force || !haveLast_;
    float norm[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i)
    {
        norm[i] = getNormalised(i);
        if (norm[i] != lastNorm_[i]) changed = true;
    }
    if (!changed) return;
    std::memcpy(lastNorm_, norm, sizeof(norm));
    haveLast_ = true;
    float plain[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i) plain[i] = get(i);
    engine_->applySettings(proposedSettings(plain));
}

void MpcBoyPlugin::resyncCursor()
{
    int vals[kEditorCols];
    store_->beginBlock();   // make sure staged content is live before reading it
    editor_.resync(store_->live(), getInt(ED_Table) - 1, getInt(ED_Row), vals);
    // Write only columns whose value changed: rewriting an unchanged one would
    // replace the host's normalised value with the canonical one and make a
    // saved chunk differ after a plain round trip.
    for (int col = 0; col < kEditorCols; ++col)
        if (getInt(ED_Vol + col) != vals[col]) set(ED_Vol + col, static_cast<float>(vals[col]));
    editor_.requestHostRefresh();
}

void MpcBoyPlugin::process(mpc::ProcessContext& ctx)
{
#ifdef MPCBOY_CPU_PROBE
    const long long t0 = nowNs();
#endif
    ContentStore::markAudioThread();

    bool forceSettings = false;
    if (resetPending_.exchange(false, std::memory_order_acq_rel))
    {
        engine_->reset();
        editor_.markCursorDirty();
        forceSettings = true;
#ifdef MPCBOY_CPU_PROBE
        probeNoteHeld_ = false;
#endif
    }

    if (ctx.numOutputs < 1 || ctx.outputs == nullptr || ctx.outputs[0] == nullptr) return;
    if (silent_.load(std::memory_order_acquire) || ctx.numOutputs < 2 || ctx.outputs[1] == nullptr)
    {
        for (int c = 0; c < ctx.numOutputs; ++c)
            if (ctx.outputs[c]) std::memset(ctx.outputs[c], 0, sizeof(float) * static_cast<size_t>(ctx.numFrames));
        return;
    }

    if (editor_.takeCursorDirty()) resyncCursor();   // also consumes staged content
    snapshotSettings(forceSettings);

    // MIDI and transport -> EngineBlock.
    int n = 0;
#ifdef MPCBOY_CPU_PROBE
    if (!probeNoteHeld_)
    {
        midi_[n].offset = 0; midi_[n].status = 0x90; midi_[n].data1 = 60; midi_[n].data2 = 100;
        ++n;
        probeNoteHeld_ = true;
    }
#endif
    for (int i = 0; i < ctx.numMidiEvents && n < kMaxMidiEvents; ++i)
    {
        const mpc::MidiEvent& e = ctx.midi[i];
        midi_[n].offset = e.sampleOffset;
        midi_[n].status = e.data[0];
        midi_[n].data1 = e.data[1];
        midi_[n].data2 = e.data[2];
        ++n;
    }
    EngineBlock blk;
    blk.midi = n > 0 ? midi_ : nullptr;
    blk.numMidi = n;
    blk.bpm = ctx.transport.bpm;
    blk.tempoValid = ctx.transport.tempoValid;
    blk.ppqPos = ctx.transport.ppqPos;
    blk.ppqValid = ctx.transport.ppqValid;
    blk.playing = ctx.transport.playing;
    engine_->render(blk, ctx.outputs[0], ctx.outputs[1], ctx.numFrames);

    // Master volume with a 10 ms smoother.
    const float target = volumeToGain(get(P_Volume));
    if (!gainInit_) { gain_ = target; gainInit_ = true; }
    float* L = ctx.outputs[0];
    float* R = ctx.outputs[1];
    if (std::fabs(target - gain_) < 1e-6f)
    {
        gain_ = target;
        for (int i = 0; i < ctx.numFrames; ++i) { L[i] *= gain_; R[i] *= gain_; }
    }
    else
    {
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            gain_ += (target - gain_) * kGainCoef;
            L[i] *= gain_;
            R[i] *= gain_;
        }
    }

    // Tell the host the cell controls moved: at most 10 Hz, from the audio thread
    // (the device wrapper sends host reports from processReplacing).
    if (editor_.refreshDue(ctx.numFrames))
    {
        for (int i = ED_Vol; i <= ED_Val2; ++i) notifyHostParameterChanged(i);
        requestDisplayUpdate();
        hostRefreshes_.fetch_add(1, std::memory_order_relaxed);
    }

#ifdef MPCBOY_CPU_PROBE
    const long long dt = nowNs() - t0;
    const uint32_t h = head_.load(std::memory_order_relaxed);
    ring_[h % kRing].store(static_cast<uint32_t>(dt > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : dt), std::memory_order_relaxed);
    head_.store(h + 1, std::memory_order_release);
#endif
}

#ifdef MPCBOY_CPU_PROBE
// Debug-only worker: once per second, mean/p99 over the last <= 20,000 blocks
// (~58 s at 128 frames). Calls requestDisplayUpdate() from here, never from process().
void MpcBoyPlugin::workerLoop()
{
    std::vector<uint32_t> snap;
    snap.reserve(kRing);
    while (!stop_.load(std::memory_order_acquire))
    {
        for (int i = 0; i < 20 && !stop_.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t n = std::min<uint32_t>(head, 20000);
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
#endif
