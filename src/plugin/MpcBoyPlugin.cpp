// MpcBoyPlugin.cpp - the mpcboy instrument (WP7).
//
// SPDX-License-Identifier: MIT
//
// process(): reset -> staged content -> Mac editor edits -> editor cursor resync -> settings
// snapshot -> Engine::render -> master volume (10 ms smoother) -> host refresh.
//
// With -DMPCBOY_CPU_PROBE=ON (debug only) the plugin additionally runs the CPU
// probe (cpu_probe.h, docs/BENCH.md); the code below calls it unconditionally.

#include "MpcBoyPlugin.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <type_traits>

#include "factory_content.h"
#include "mpc_resources.h"
#include "state_codec.h"

using namespace mpcboy;

namespace {

// pthread_self() as an integer (as ContentStore does: no TLS, no allocation).
template <typename T>
uintptr_t toId(T t, std::true_type) { return reinterpret_cast<uintptr_t>(t); }
template <typename T>
uintptr_t toId(T t, std::false_type) { return static_cast<uintptr_t>(t); }
uintptr_t selfId() { return toId(pthread_self(), std::is_pointer<pthread_t>()); }

constexpr uint64_t kCellValid = 1ull << 63;

uint64_t packCell(int table, int row, int value)
{
    return kCellValid | (static_cast<uint64_t>(static_cast<uint8_t>(row)) << 40) |
           (static_cast<uint64_t>(static_cast<uint8_t>(table)) << 32) | static_cast<uint32_t>(value);
}

// 10 ms one-pole smoother at the given host rate (44.1 kHz: the original constant).
float gainCoefFor(int sampleRate)
{
    return 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(sampleRate)));
}

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
    i.latencySamples = 0;                          // resampler delay (117.6 host frames at 44.1k, 0 at 48k, 256 at 96k, 512 at 192k) is documented, not reported (R7)
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
    // The import worker scans Content/Import and imports on request; started
    // last, joined first (members are destroyed in reverse order).
    importer_ = std::make_unique<ImportWorker>(mpc::contentPath("Import"), *store_);

    for (const mpc::Param& p : buildParams()) addParam(p);
    for (const ProgramDef& d : programDefs()) addProgram(d.name, d.values);

    float plain[kNumEngineParams];
    for (int i = 0; i < kNumEngineParams; ++i) plain[i] = get(i);
    engine_->applySettings(proposedSettings(plain));
    gain_ = volumeToGain(get(P_Volume));
    gainCoef_ = gainCoefFor(44100);

    probe_.start();   // probe builds only: after the programs exist
}

MpcBoyPlugin::~MpcBoyPlugin()
{
    importer_.reset();   // joins the worker before the content store goes away
}

void MpcBoyPlugin::prepare(double sampleRate, int)
{
    // May run on the UI thread while process() runs: only publish the rate (0 for
    // a nonsense value). process() applies it. Supported: 44.1, 48, 96, 192 kHz
    // (Engine::hostRateSupported); anything else (88.2 kHz, ...) renders silence.
    const long long r = std::llround(sampleRate);
    requestedRate_.store(r > 0 && r < 1000000 ? static_cast<int>(r) : 0, std::memory_order_release);
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
        if (editorWriter_.load(std::memory_order_relaxed) == selfId()) return;   // the drain applies it
        editor_.onCellParam(index - ED_Vol, static_cast<int>(std::lround(plainValue)),
                            getInt(ED_Table) - 1, getInt(ED_Row), *store_);
    }
    else if (index >= Imp_Go && index <= Imp_Rescan)
    {
        // Any thread; an edge only acts when it arrives on the audio thread (see ImportTriggers).
        triggers_.onValue(index - Imp_Go, plainValue, store_->onAudioThread());
    }
}

void MpcBoyPlugin::setClockForTest(NowFn fn) { triggers_.setClock(fn); }

// ---- editor edits (main thread) ---------------------------------------------------------------
void MpcBoyPlugin::sendHost(int opcode, int index, float opt)
{
    if (host_ && effect_) host_(effect_, opcode, index, 0, nullptr, opt);
}

void MpcBoyPlugin::editorBeginEdit(int index)
{
    if (index < 0 || index >= numParams() || editOpen_[index]) return;
    editOpen_[index] = true;
    if (index < kNumEngineParams)
    {
        sendHost(audioMasterBeginEdit, index, 0.0f);
    }
    else if (index >= ED_Vol && index <= ED_Val2)
    {
        const int col = index - ED_Vol;
        capTable_[col] = getInt(ED_Table) - 1;
        capRow_[col] = getInt(ED_Row);
        capValid_[col] = true;
    }
}

void MpcBoyPlugin::editorSet(int index, float norm)
{
    if (index < 0 || index >= numParams()) return;
    if (index < kNumEngineParams)
    {
        setNormalised(index, norm);
        sendHost(audioMasterAutomate, index, getNormalised(index));
    }
    else if (index >= ED_Vol && index <= ED_Val2)
    {
        const int col = index - ED_Vol;
        const int table = capValid_[col] ? capTable_[col] : getInt(ED_Table) - 1;
        const int row = capValid_[col] ? capRow_[col] : getInt(ED_Row);
        editorWriter_.store(selfId(), std::memory_order_relaxed);
        setNormalised(index, norm);   // display only
        editorWriter_.store(0, std::memory_order_relaxed);
        cellSlots_[col].store(packCell(table, row, getInt(index)), std::memory_order_release);
    }
    else if (index >= Imp_Go && index <= Imp_Rescan)
    {
        if (norm < 0.5f)
        {
            setNormalised(index, 0.0f);
            return;
        }
        setNormalised(index, 1.0f);   // shown on until process() writes it back to 0
        triggerSlots_[index - Imp_Go].store(true, std::memory_order_release);
    }
    else
    {
        setNormalised(index, norm);   // Edit Table, Edit Row, Imp File: as a host write
    }
}

void MpcBoyPlugin::editorEndEdit(int index)
{
    if (index < 0 || index >= numParams() || !editOpen_[index]) return;
    editOpen_[index] = false;
    if (index < kNumEngineParams) sendHost(audioMasterEndEdit, index, 0.0f);
    else if (index >= ED_Vol && index <= ED_Val2) capValid_[index - ED_Vol] = false;
}

void MpcBoyPlugin::endAllEditorEdits()
{
    for (int i = 0; i < numParams(); ++i) editorEndEdit(i);
}

bool MpcBoyPlugin::editorEditOpen(int index) const
{
    return index >= 0 && index < numParams() && editOpen_[index];
}

// Audio thread: the editor's cell edits land on the cell captured at click time; its triggers
// are accepted with the debounce. Then the cursor resyncs (the edit may have hit another row).
void MpcBoyPlugin::drainEditor()
{
    for (int col = 0; col < kEditorCols; ++col)
    {
        const uint64_t v = cellSlots_[col].exchange(0, std::memory_order_acq_rel);
        if (!(v & kCellValid)) continue;
        const int table = static_cast<int>((v >> 32) & 0xff);
        const int row = static_cast<int>((v >> 40) & 0xff);
        const int value = static_cast<int32_t>(static_cast<uint32_t>(v & 0xffffffffu));
        editor_.applyCell(col, value, table, row, *store_);
        editor_.markCursorDirty();
    }
    for (int k = 0; k < kNumImportTriggers; ++k)
        if (triggerSlots_[k].exchange(false, std::memory_order_acq_rel)) triggers_.acceptFromEditor(k);
}

// Audio thread: write the triggers back to 0, follow the worker's list and its
// suggested selection, and ask for a host refresh (the existing <= 10 Hz path).
void MpcBoyPlugin::serviceImport()
{
    for (int k = 0; k < kNumImportTriggers; ++k)
    {
        if (triggers_.takePending(k))
        {
            switch (Imp_Go + k)
            {
                case Imp_Go: importer_->requestImport(getInt(Imp_File) - 1); break;   // ignored while one is running
                case Imp_Undo: store_->requestUndo(); break;
                case Imp_Rescan: importer_->requestScan(); break;
                default: break;
            }
        }
    }
    for (int k = 0; k < kNumImportTriggers; ++k)
        if (triggers_.takeReset(k))
        {
            set(Imp_Go + k, 0.0f);
            editor_.requestHostRefresh();
        }
    const uint32_t v = importer_->version();
    if (v != seenImportVersion_)
    {
        seenImportVersion_ = v;
        editor_.requestHostRefresh();
    }
    const int s = importer_->takeSuggestedIndex();
    if (s >= 0)
    {
        set(Imp_File, static_cast<float>(s + 1));
        editor_.requestHostRefresh();
    }
}

void MpcBoyPlugin::saveExtraState(std::vector<uint8_t>& out)
{
    Content c;
    ContentSource src;
    store_->readLatest(c, &src);
    encodeState(c, out, std::string(src.name));
}

void MpcBoyPlugin::loadExtraState(const uint8_t* data, size_t size)
{
    triggers_.cancelPending();   // a restored 1 must not act
    Content c;
    std::string name;
    if (!decodeState(data, size, c, &name)) return;   // corrupt or foreign: keep the current content
    importer_->setPreferredName(name);   // the worker selects it in Imp File once it is in the list
    store_->stage(c, name.c_str());
}

bool MpcBoyPlugin::formatValue(int index, float plainValue, std::string& out)
{
    if (probe_.format(index, out)) return true;
    if (index == Imp_File)
    {
        // No mutex, no allocation (this can run inside process() via the host
        // refresh): the text is at most 15 characters, which fits std::string's
        // small buffer.
        char buf[ImportWorker::kDisplayBufSize];
        importer_->displayTextInto(static_cast<int>(std::lround(plainValue)) - 1, buf);
        out.assign(buf);
        return true;
    }
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
    const long long probeT0 = probe_.begin();
    store_->markAudioThread();

    bool forceSettings = false;
    bool doReset = resetPending_.exchange(false, std::memory_order_acq_rel);
    const int rate = requestedRate_.load(std::memory_order_acquire);
    if (rate != appliedRate_)
    {
        appliedRate_ = rate;
        silent_ = !engine_->setHostRate(rate);   // unsupported: the engine keeps its old rate, we output silence
        if (!silent_)
        {
            gainCoef_ = gainCoefFor(rate);
            editor_.setSampleRate(rate);
        }
        doReset = true;
    }
    if (doReset)
    {
        engine_->reset();
        editor_.markCursorDirty();
        forceSettings = true;
        probe_.onReset();
    }

    serviceImport();

    if (ctx.numOutputs < 1 || ctx.outputs == nullptr || ctx.outputs[0] == nullptr)
    {
        drainEditor();   // editor edits still land (published by the next full block)
        return;
    }
    if (silent_ || ctx.numOutputs < 2 || ctx.outputs[1] == nullptr)
    {
        drainEditor();
        for (int c = 0; c < ctx.numOutputs; ++c)
            if (ctx.outputs[c]) std::memset(ctx.outputs[c], 0, sizeof(float) * static_cast<size_t>(ctx.numFrames));
        return;
    }

    // The single owner of "consume staged content" (load / import / undo): the
    // cursor resync, the settings snapshot and Engine::render below all see the
    // content this call made live.
    store_->beginBlock();
    if (store_->bulkGeneration() != seenBulkGen_)
    {
        seenBulkGen_ = store_->bulkGeneration();
        editor_.markCursorDirty();
    }
    drainEditor();
    if (editor_.takeCursorDirty()) resyncCursor();
    snapshotSettings(forceSettings);

    // MIDI and transport -> EngineBlock.
    int n = 0;
    n += probe_.injectNote(midi_);
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
            gain_ += (target - gain_) * gainCoef_;
            L[i] *= gain_;
            R[i] *= gain_;
        }
    }

    // Tell the host the cell controls moved: at most 10 Hz, from the audio thread
    // (the device wrapper sends host reports from processReplacing).
    if (editor_.refreshDue(ctx.numFrames))
    {
        for (int i = ED_Vol; i <= Imp_Rescan; ++i) notifyHostParameterChanged(i);
        requestDisplayUpdate();
        hostRefreshes_.fetch_add(1, std::memory_order_relaxed);
    }

    probe_.end(probeT0);
}
