#include "editor_sync.h"

#include <algorithm>

#include "param_bridge.h"
#include "rackboy_sav_tables.h"

namespace mpcboy {

int EditorSync::cellValue(const rackboy::sav::RackboyTableRow& r, int col)
{
    switch (col)
    {
        case 0: return r.has_vol ? r.vol : -1;
        case 1: return std::clamp<int>(r.transpose, -48, 48);
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
            else { r.has_vol = true; r.vol = static_cast<uint8_t>(std::clamp<int>(v, 0, 255)); }
            break;
        case 1: r.transpose = static_cast<int8_t>(std::clamp<int>(v, -48, 48)); break;
        case 2: r.cmd1 = cmdByteFromIndex(v); break;
        case 3: r.cmd1_param = static_cast<uint8_t>(std::clamp<int>(v, 0, 255)); break;
        case 4: r.cmd2 = cmdByteFromIndex(v); break;
        default: r.cmd2_param = static_cast<uint8_t>(std::clamp<int>(v, 0, 255)); break;
    }
}

bool EditorSync::onCellParam(int col, int value, int table, int row, ContentStore& store)
{
    if (col < 0 || col >= kEditorCols) return false;
    if (value == shown(col)) return false;
    if (!store.onAudioThread()) return false;
    const int t = std::clamp<int>(table, 0, kNumTables - 1);
    const int r = std::clamp<int>(row, 0, static_cast<int>(rackboy::sav::kTableRowsPerTable) - 1);
    const bool applied = store.editLive([&](Content& c) { setCellValue(c.tables.tables[t].rows[r], col, value); });
    if (applied) setShown(col, value);
    return applied;
}

bool EditorSync::applyCell(int col, int value, int table, int row, ContentStore& store)
{
    if (col < 0 || col >= kEditorCols) return false;
    const int t = std::clamp<int>(table, 0, kNumTables - 1);
    const int r = std::clamp<int>(row, 0, static_cast<int>(rackboy::sav::kTableRowsPerTable) - 1);
    return store.editLive([&](Content& c) { setCellValue(c.tables.tables[t].rows[r], col, value); });
}

void EditorSync::resync(const Content& live, int table, int row, int out[kEditorCols])
{
    const auto& cell = live.tables.tables[std::clamp<int>(table, 0, kNumTables - 1)].rows[std::clamp<int>(row, 0, static_cast<int>(rackboy::sav::kTableRowsPerTable) - 1)];
    for (int col = 0; col < kEditorCols; ++col)
    {
        out[col] = cellValue(cell, col);
        setShown(col, out[col]);
    }
}

}  // namespace mpcboy
