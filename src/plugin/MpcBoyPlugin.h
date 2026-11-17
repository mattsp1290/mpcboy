// src/plugin/MpcBoyPlugin.h - the mpc::Plugin subclass (WP7).
//
// Threading (see 04-plugin-params-ui-state.md and src/engine/content_store.h):
//   audio thread  process(), parameter writes delivered between blocks
//   UI thread     prepare(), reset(), loadExtraState(), saveExtraState(), formatValue()
// reset() and prepare() only set atomic flags; loadExtraState() stages content;
// saveExtraState() reads the published snapshot.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#ifdef MPCBOY_CPU_PROBE
#include <thread>
#endif

#include "content_store.h"
#include "editor_sync.h"
#include "engine.h"
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

private:
    void onImportTrigger(int index, float plainValue);
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

    // Import triggers (WP11). trigHigh_: the parameter is currently on (edge
    // detection); trigReset_: process() must write it back to 0; trigLastNs_:
    // time of the last accepted edge (250 ms debounce).
    std::atomic<bool> trigHigh_[mpcboy::kNumImportTriggers];
    std::atomic<bool> trigReset_[mpcboy::kNumImportTriggers];
    std::atomic<long long> trigLastNs_[mpcboy::kNumImportTriggers];

    // Audio thread only.
    float lastNorm_[mpcboy::kNumEngineParams];
    bool haveLast_ = false;
    float gain_ = 1.0f;
    bool gainInit_ = false;
    uint32_t seenBulkGen_ = 0;
    uint32_t seenImportVersion_ = 0;
    mpcboy::EngineMidiEvent midi_[kMaxMidiEvents + 1];

#ifdef MPCBOY_CPU_PROBE
    void workerLoop();
    static constexpr uint32_t kRing = 32768;   // ~95 s of 128-frame blocks
    std::atomic<uint32_t> ring_[kRing];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> meanX10_{0};
    std::atomic<uint32_t> p99X10_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    bool probeNoteHeld_ = false;
#endif
};
