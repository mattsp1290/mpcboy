#include "editor_sync.h"

#include "param_bridge.h"

namespace mpcboy {

namespace {
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

int EditorSync::cellValue(const rackboy::sav::RackboyTableRow& r, int col)
{
    switch (col)
    {
        case 0: return r.has_vol ? r.vol : -1;
        case 1: return clampi(r.transpose, -48, 48);
        case 2: return cmdIndexFromByte(r.cmd1);
        case 3: return r.cmd1_param;
        case 4: return cmdIndexFromByte(r.cmd2);
        default: return r.cmd2_param;
    }
}

void EditorSync::setCellValue(rackboy::sav::RackboyTableRow& r, int col, int v)
{
    switch (col)
    {
        case 0:
            if (v < 0) { r.has_vol = false; r.vol = 0; }
            else { r.has_vol = true; r.vol = static_cast<uint8_t>(clampi(v, 0, 255)); }
            break;
        case 1: r.transpose = static_cast<int8_t>(clampi(v, -48, 48)); break;
        case 2: r.cmd1 = cmdByteFromIndex(v); break;
        case 3: r.cmd1_param = static_cast<uint8_t>(clampi(v, 0, 255)); break;
        case 4: r.cmd2 = cmdByteFromIndex(v); break;
        default: r.cmd2_param = static_cast<uint8_t>(clampi(v, 0, 255)); break;
    }
}

bool EditorSync::onCellParam(int col, int value, int table, int row, ContentStore& store)
{
    if (col < 0 || col >= kEditorCols) return false;
    if (value == shown(col)) return false;
    if (!ContentStore::onAudioThread()) return false;
    const int t = clampi(table, 0, kNumTables - 1);
    const int r = clampi(row, 0, 15);
    const bool applied = store.editLive([&](Content& c) { setCellValue(c.tables.tables[t].rows[r], col, value); });
    if (applied) setShown(col, value);
    return applied;
}

void EditorSync::resync(const Content& live, int table, int row, int out[kEditorCols])
{
    const auto& cell = live.tables.tables[clampi(table, 0, kNumTables - 1)].rows[clampi(row, 0, 15)];
    for (int col = 0; col < kEditorCols; ++col)
    {
        out[col] = cellValue(cell, col);
        setShown(col, out[col]);
    }
}

}  // namespace mpcboy
