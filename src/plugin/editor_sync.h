// src/plugin/editor_sync.h - table editor cursor logic (WP7), testable without
// the host. The plugin owns the parameters; this class owns the filter state.
//
// Columns (index into the six cell parameters ED_Vol .. ED_Val2):
//   0 vol (-1 = none, else 0..255)   1 transpose (-48..48)
//   2 cmd1 (choice index)            3 val1 (0..255)
//   4 cmd2 (choice index)            5 val2 (0..255)
//
// Threading: markCursorDirty / takeCursorDirty / shown / setShown are atomic.
// The refresh bookkeeping (needsRefresh_, framesSince_) is audio-thread only.
#pragma once

#include <atomic>
#include <climits>

#include "content_store.h"
#include "rackboy_sav_types.h"
#include "table_bank.h"

namespace mpcboy {

constexpr int kEditorCols = 6;
// Host refresh cadence: at most one refresh per 100 ms (10 Hz): 4,410 frames at
// 44.1 kHz, sampleRate / 10 in general (setSampleRate).
constexpr int kRefreshIntervalFrames = 4410;

class EditorSync
{
public:
    EditorSync()
    {
        for (auto& s : shown_) s.store(INT_MIN, std::memory_order_relaxed);
    }

    // ---- pure cell <-> value conversion ------------------------------------
    static int cellValue(const rackboy::sav::RackboyTableRow& row, int col);
    static void setCellValue(rackboy::sav::RackboyTableRow& row, int col, int value);

    // ---- cursor ---------------------------------------------------------------
    // parameterChanged(ED_Table / ED_Row), any thread.
    void markCursorDirty() { cursorDirty_.store(true, std::memory_order_release); }
    // Audio thread: true once per pending change (cleared by the call).
    bool takeCursorDirty() { return cursorDirty_.exchange(false, std::memory_order_acq_rel); }

    int shown(int col) const { return shown_[col].load(std::memory_order_relaxed); }
    void setShown(int col, int v) { shown_[col].store(v, std::memory_order_relaxed); }

    // parameterChanged(ED_Vol..ED_Val2, value). Returns true when a cell was
    // written. Ignored when the value equals what the editor last showed (the
    // plugin's own resync writes and host echoes) and when the caller is not on
    // the audio thread (WP5 rule 4; ContentStore::editLive enforces it).
    bool onCellParam(int col, int value, int table, int row, ContentStore& store);

    // Audio thread: a Mac editor cell edit for an explicit cell (the cursor captured at click
    // time). Unlike onCellParam there is no "shown" filter: the value is the user's, for a cell
    // that need not be under the cursor any more. Returns whether it was applied.
    bool applyCell(int col, int value, int table, int row, ContentStore& store);

    // Audio thread: the six values of the cell under the cursor. Stores them as
    // "shown" first (so the parameter writes that follow are filtered).
    void resync(const Content& live, int table, int row, int out[kEditorCols]);

    // ---- host refresh (audio thread) -----------------------------------------
    void requestHostRefresh() { needsRefresh_ = true; }
    // Adds `frames` to the clock; returns true when a refresh is due now (and
    // resets the clock and the request).
    void setSampleRate(int sr) { interval_ = sr >= 1000 ? sr / 10 : kRefreshIntervalFrames; }
    bool refreshDue(int frames)
    {
        if (framesSince_ < interval_) framesSince_ += frames;
        if (needsRefresh_ && framesSince_ >= interval_)
        {
            needsRefresh_ = false;
            framesSince_ = 0;
            return true;
        }
        return false;
    }

private:
    std::atomic<int> shown_[kEditorCols];
    std::atomic<bool> cursorDirty_{true};
    bool needsRefresh_ = false;
    int interval_ = kRefreshIntervalFrames;
    int framesSince_ = kRefreshIntervalFrames;
};

}  // namespace mpcboy
