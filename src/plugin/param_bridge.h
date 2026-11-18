// src/plugin/param_bridge.h - the parameter table (WP7) and the pure mapping
// from a parameter snapshot to EngineSettings.
//
// Index order is fixed by the enum Params and matches
// .agents/plans/mpcboy-gb-synth/04-plugin-params-ui-state.md ("Parameter table").
// Names are what the MPC shows and what ui/mpcboy_ui.json references.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "channel_settings.h"
#include "mpc_plugin.h"
#include "table_bank.h"

namespace mpcboy {

enum Params : int
{
    P_BendRange = 0,
    P_Volume,
    P_Retrigger,
    P_NoteOff,
    P_VelVol,
    P_TblClock,
    P_TblRate,
    P_TblPhase,
    P_FreeHz,
    // Pulse 1 (9..21)
    P1_On, P1_Vol, P1_EnvDir, P1_EnvSpd, P1_Duty, P1_Tsp, P1_Fine, P1_Pan, P1_Len,
    P1_SwTime, P1_SwDir, P1_SwShift, P1_Tbl,
    // Pulse 2 (22..31)
    P2_On, P2_Vol, P2_EnvDir, P2_EnvSpd, P2_Duty, P2_Tsp, P2_Fine, P2_Pan, P2_Len, P2_Tbl,
    // Wave (32..39)
    WV_On, WV_Level, WV_Wave, WV_Tsp, WV_Fine, WV_Pan, WV_Len, WV_Tbl,
    // Noise (40..50)
    NZ_On, NZ_Vol, NZ_EnvDir, NZ_EnvSpd, NZ_Width, NZ_PitchMode, NZ_Shape, NZ_Tsp, NZ_Pan, NZ_Len, NZ_Tbl,
    // Editor cursor and cell (51..58): UI plumbing, not automatable, never in a program.
    ED_Table, ED_Row, ED_Vol, ED_Tsp, ED_Cmd1, ED_Val1, ED_Cmd2, ED_Val2,
    // .sav import (59..62, WP11): UI plumbing too. Imp_Go / Imp_Undo / Imp_Rescan
    // are triggers (a 0->1 edge acts; the plugin sets them back to 0).
    Imp_File, Imp_Go, Imp_Undo, Imp_Rescan,
    kNumParams,
    // Debug CPU probe parameter, appended last, only in -DMPCBOY_CPU_PROBE=ON builds.
    P_DbgUs = kNumParams
};

constexpr int kNumEngineParams = ED_Table;   // 51: programs list values for 0..50 only
constexpr int kFirstEditorParam = ED_Table;
constexpr int kNumImportTriggers = Imp_Rescan - Imp_Go + 1;   // Imp_Go .. Imp_Rescan

#ifdef MPCBOY_CPU_PROBE
constexpr int kTotalParams = kNumParams + 1;
#else
constexpr int kTotalParams = kNumParams;
#endif

// LSDJ table commands offered by Row Cmd1/Cmd2: kCmdLetters in table_bank.h.
// Unknown bytes (possible after a .sav import) map to index 0.
int cmdIndexFromByte(uint8_t b);
uint8_t cmdByteFromIndex(int index);

// All parameters in index order (plus the probe parameter when enabled).
std::vector<mpc::Param> buildParams();

struct ProgramDef
{
    const char* name;
    std::vector<float> values;   // plain values for parameters 0..50
};
// The eight built-in programs: Init, ChipLead, MajArp, WaveBass, Kick, Hat, Stack4, Zap.
const std::vector<ProgramDef>& programDefs();
constexpr int kProgramStack4 = 6;

// `plain` holds the plain values of parameters 0..kNumEngineParams-1.
// Volume is not an engine setting: the plugin applies it through its own smoother.
EngineSettings proposedSettings(const float* plain);

// Linear gain of the Volume parameter (dB).
float volumeToGain(float db);

// NOI FixPitch: N values, 0..N-1, sorted by noise clock frequency ascending with duplicate
// frequencies collapsed (strictly increasing pitch). Register index = NR43 shift * 8 + divisor code.
int noiseFixPitchCount();
int noiseFixPitchRegIndex(int value);
double noiseFixPitchHz(int value);
int noiseFixPitchValueNear(int regIndex);   // the value closest in frequency to a register index

// Pure function of the value (no cell lookups). Returns false for parameters
// that use the default formatting.
bool formatParamValue(int index, float plain, std::string& out);

}  // namespace mpcboy
