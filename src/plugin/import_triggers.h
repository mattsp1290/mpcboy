// src/plugin/import_triggers.h - the three .sav import trigger parameters
// (Imp Go / Imp Undo / Imp Rescan): edge detection, the 250 ms debounce and the
// hand-off from whichever thread delivered the edge to process().
//
// Rules (WP11):
//   - a 0 -> 1 edge acts once; a second edge within 250 ms is ignored (one tap
//     can arrive as two presses on the device);
//   - an edge always schedules a write-back of the parameter to 0 (takeReset);
//   - only an edge delivered on the audio thread can act (takePending): screen
//     taps arrive there, while project and preset loads run on the UI thread and
//     a trigger restored as 1 by a load must not start an import or undo;
//   - a state load cancels accepted-but-unserviced edges (cancelPending): the
//     SDK writes the parameters of a chunk before loadExtraState().
//
// Lock-free; any thread may call onValue().
#pragma once

#include <atomic>

#include "param_bridge.h"

namespace mpcboy {

class ImportTriggers
{
public:
    using NowFn = long long (*)();

    ImportTriggers();

    // Time source of the debounce (steady_clock nanoseconds by default). Tests
    // inject a fake clock; nullptr restores the default.
    void setClock(NowFn fn);

    // Any thread. `k` = 0 .. kNumImportTriggers - 1 (Imp_Go + k).
    void onValue(int k, float plain, bool onAudioThread);

    // Audio thread: an edge from the Mac editor (drained by process(), so it is on the audio
    // thread). Accepted like an audio-thread edge (the 250 ms debounce applies) but without
    // edge detection: the editor's own main-thread setNormalised(1) already set `high`, which
    // would otherwise swallow it. Returns whether it was accepted.
    bool acceptFromEditor(int k);

    // Audio thread: consume an accepted edge / a pending write-back to 0.
    bool takePending(int k) { return slots_[k].pending.exchange(false, std::memory_order_acq_rel); }
    bool takeReset(int k) { return slots_[k].reset.exchange(false, std::memory_order_acq_rel); }

    // Discard accepted edges that were not serviced yet (write-backs stay).
    void cancelPending();

private:
    bool accept(int k);   // debounce, then mark pending

    struct Slot
    {
        std::atomic<bool> high{false};         // the parameter is on (edge detection)
        std::atomic<bool> reset{false};        // process() must write it back to 0
        std::atomic<bool> pending{false};      // an accepted edge waits for process()
        std::atomic<long long> lastNs{0};      // time of the last accepted edge
    };

    Slot slots_[kNumImportTriggers];
    std::atomic<NowFn> now_;
};

}  // namespace mpcboy
