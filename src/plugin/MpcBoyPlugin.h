// src/plugin/MpcBoyPlugin.h - the mpc::Plugin subclass (WP7).
//
// Threading (see 04-plugin-params-ui-state.md and src/engine/content_store.h):
//   audio thread  process(), parameter writes delivered between blocks
//   UI thread     prepare(), reset(), loadExtraState(), saveExtraState(), formatValue()
// reset() and prepare() only set atomic values (prepare: the host rate, applied by process()); loadExtraState() stages content;
// saveExtraState() reads the published snapshot.
// "The audio thread" is the thread that last ran process() (ContentStore::
// markAudioThread: a sticky per-store thread id, never cleared). Editor-cell and
// import-trigger parameter writes only act when they arrive on that thread; in
// the native tests the thread that calls process() therefore stays "the audio
// thread" of that plugin instance for its whole life.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "content_store.h"
#include "cpu_probe.h"
#include "editor_sync.h"
#include "engine.h"
#include "import_triggers.h"
#include "import_worker.h"
#include "mpc_plugin.h"
#include "param_bridge.h"

class MpcBoyPlugin : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo();

    MpcBoyPlugin();
    ~MpcBoyPlugin() override;

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(mpc::ProcessContext& ctx) override;
    void parameterChanged(int index, float plainValue) override;
    void saveExtraState(std::vector<uint8_t>& out) override;
    void loadExtraState(const uint8_t* data, size_t size) override;
    bool formatValue(int index, float plainValue, std::string& out) override;

    // ---- editor edits (the Mac editor; main thread) -----------------------------------------
    // See .agents/plans/mpcboy-gb-ui/00-design.md "Thread and edit contract".
    //   0..50   editorBeginEdit / editorSet / editorEndEdit send audioMasterBeginEdit /
    //           setNormalised + audioMasterAutomate / audioMasterEndEdit. Begin and end are
    //           paired per parameter (a second begin or an end without a begin sends nothing);
    //           endAllEditorEdits() ends whatever is open (editor close, cancelled tracking).
    //   53..58  (cells) setNormalised for display, then a per-column atomic slot holding
    //           {table, row, value}: the cursor captured at editorBeginEdit (or at the set when
    //           no edit is open). process() applies it to that cell, even if the cursor moved.
    //   60..62  (triggers) setNormalised(1) for display, then a per-trigger flag; process()
    //           accepts it through ImportTriggers::acceptFromEditor (debounced).
    //   51, 52, 59  setNormalised, as a host write (the cursor resyncs; the file is picked).
    // 51..62 never send begin / automate / end: they are not automatable.
    void editorBeginEdit(int index);
    void editorSet(int index, float norm);
    void editorEndEdit(int index);
    void endAllEditorEdits();
    bool editorEditOpen(int index) const;
    // Live table playback per channel (any thread; relaxed atomics written once per block by the
    // audio thread). The editor's channel LCDs and TABLE grid read it at 30 Hz.
    const mpcboy::PlaybackState& playback() const { return engine_->playback(); }

    // Tests.
    mpcboy::ContentStore& content() { return *store_; }
    uint32_t hostRefreshCount() const { return hostRefreshes_.load(std::memory_order_relaxed); }
    mpcboy::ImportWorker& importer() { return *importer_; }
    mpcboy::Engine& engine() { return *engine_; }   // tests only (register-write spy)
    // Time source of the import-trigger debounce (steady_clock nanoseconds by
    // default). Tests inject a fake clock; pass nullptr to restore the default.
    using NowFn = mpcboy::ImportTriggers::NowFn;
    void setClockForTest(NowFn fn);

private:
    void serviceImport();   // audio thread: trigger resets, list changes
    void snapshotSettings(bool force);
    void resyncCursor();
    void drainEditor();      // audio thread: apply the editor's cell edits and triggers
    void drainEarly();       // audio thread: drainEditor() for the early-return paths, then publish
    void sendHost(int opcode, int index, float opt);

    // Editor -> audio thread slots (latest wins, any number of producers, never overflow).
    // Cell slot: bit 63 valid, bits 40..47 row, 32..39 table (0-based), 0..31 value (int32).
    std::atomic<uint64_t> cellSlots_[mpcboy::kEditorCols] = {};
    std::atomic<bool> triggerSlots_[mpcboy::kNumImportTriggers] = {};
    // The thread inside editorSet's setNormalised on a cell (0 = none): parameterChanged then
    // leaves the cell to the drain, so an editor that runs on the audio thread cannot write the
    // current cursor's cell instead of the captured one.
    std::atomic<uintptr_t> editorWriter_{0};
    // Editor thread only.
    bool editOpen_[mpcboy::kTotalParams] = {};
    int capTable_[mpcboy::kEditorCols] = {};
    int capRow_[mpcboy::kEditorCols] = {};
    bool capValid_[mpcboy::kEditorCols] = {};

    std::unique_ptr<mpcboy::Engine> engine_;
    std::unique_ptr<mpcboy::ContentStore> store_;
    std::unique_ptr<mpcboy::ImportWorker> importer_;   // after store_: joined first
    mpcboy::EditorSync editor_;

    // prepare() (any thread) publishes the requested host rate; process() applies it
    // on the audio thread (engine rate, tick clock, gain smoother, refresh interval)
    // together with an engine reset, so nothing races or allocates.
    std::atomic<int> requestedRate_{44100};
    std::atomic<bool> resetPending_{false};
    std::atomic<uint32_t> hostRefreshes_{0};

    mpcboy::ImportTriggers triggers_;   // WP11 import trigger parameters

    // Audio thread only.
    float lastNorm_[mpcboy::kNumEngineParams];
    bool haveLast_ = false;
    float gain_ = 1.0f;
    bool gainInit_ = false;
    float gainCoef_ = 0.0f;       // 10 ms one-pole at the applied host rate
    int appliedRate_ = 44100;     // the engine starts at 44.1 kHz
    bool silent_ = false;         // applied rate unsupported: output silence
    uint32_t seenBulkGen_ = 0;
    uint32_t seenImportVersion_ = 0;
    mpcboy::EngineMidiEvent midi_[kMaxMidiEvents + 1];

    // Declared last: destroyed first (joins the probe thread in probe builds).
    mpcboy::CpuProbe probe_{*this};
};
