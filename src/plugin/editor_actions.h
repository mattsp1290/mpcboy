// src/plugin/editor_actions.h - Mac editor click actions that are not a ui::Gesture, as plain
// functions over the plugin's editor edit API (so the native tests drive the same path the
// editor does). Editor (main) thread only. See 00-design.md "Thread and edit contract".
#pragma once

#include <cmath>

#include "MpcBoyPlugin.h"
#include "param_bridge.h"

namespace mpcboy {

// A discrete edit: begin, one value, end (0..50 send begin / automate / end; the editor
// parameters 51..62 send nothing to the host, see MpcBoyPlugin::editorSet).
inline void editorClickSet(MpcBoyPlugin& p, int index, float norm)
{
    if (index < 0 || index >= p.numParams()) return;
    p.editorBeginEdit(index);
    p.editorSet(index, norm);
    p.editorEndEdit(index);
}

// TABLE grid: a click on row `row` (0..15) moves the cursor: Edit Row (52) is set like a host
// write, and the audio thread resyncs Row Vol .. Row Val2 from that row.
inline void editorSelectRow(MpcBoyPlugin& p, int row)
{
    if (row < 0 || row > 15) return;
    editorClickSet(p, ED_Row, p.param(ED_Row).toNormalised(static_cast<float>(row)));
}

// MAIN header-band On toggles: flip a two-state parameter (begin / automate / end).
inline void editorToggle(MpcBoyPlugin& p, int index)
{
    if (index < 0 || index >= p.numParams()) return;
    editorClickSet(p, index, p.getNormalised(index) >= 0.5f ? 0.0f : 1.0f);
}

}  // namespace mpcboy
