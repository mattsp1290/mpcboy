// src/plugin/MpcBoyPlugin.h - the mpc::Plugin subclass (WP7).
//
// Threading (see 04-plugin-params-ui-state.md and src/engine/content_store.h):
//   audio thread  process(), parameter writes delivered between blocks
//   UI thread     prepare(), reset(), loadExtraState(), saveExtraState(), formatValue()
// reset() and prepare() only set atomic flags; loadExtraState() stages content;
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

    std::unique_ptr<mpcboy::Engine> engine_;
    std::unique_ptr<mpcboy::ContentStore> store_;
    std::unique_ptr<mpcboy::ImportWorker> importer_;   // after store_: joined first
    mpcboy::EditorSync editor_;

    std::atomic<bool> silent_{false};
    std::atomic<bool> resetPending_{false};
    std::atomic<uint32_t> hostRefreshes_{0};

    mpcboy::ImportTriggers triggers_;   // WP11 import trigger parameters

    // Audio thread only.
    float lastNorm_[mpcboy::kNumEngineParams];
    bool haveLast_ = false;
    float gain_ = 1.0f;
    bool gainInit_ = false;
    uint32_t seenBulkGen_ = 0;
    uint32_t seenImportVersion_ = 0;
    mpcboy::EngineMidiEvent midi_[kMaxMidiEvents + 1];

    // Declared last: destroyed first (joins the probe thread in probe builds).
    mpcboy::CpuProbe probe_{*this};
};
