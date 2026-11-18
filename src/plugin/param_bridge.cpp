#include "param_bridge.h"

#include "cpu_probe.h"
#include "editor_sync.h"
#include "factory_content.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mpcboy {

static_assert(kEditorCols == ED_Val2 - ED_Vol + 1, "editor columns are ED_Vol .. ED_Val2");

namespace {

const std::vector<std::string> kDutyLabels = {"12.5%", "25%", "50%", "75%"};
// Choice labels are static (the MPC skin's replacementStrings carry them) and at most 8 characters.
const std::vector<std::string> kPanLabels = {"Mute", "Left", "Both", "Right"};
const std::vector<std::string> kWaveLevelLabels = {"Mute", "25%", "50%", "100%"};
// Down first, for Env Dir and Swp Dir alike. The engine flags are envUp / sweepDown (see readChannel
// and proposedSettings).
const std::vector<std::string> kEnvDirLabels = {"Down", "Up"};
const std::vector<std::string> kSwpDirLabels = {"Down", "Up"};

std::string slotLabel(const char* factory, const char* prefix, int slot)
{
    if (factory && factory[0]) return factory;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s %02d", prefix, slot + 1);   // user slots: "Table 09", "Wave 09"
    return buf;
}

std::vector<std::string> tableLabels()
{
    std::vector<std::string> v = {"Off"};
    for (int i = 0; i < kNumTables; ++i) v.push_back(slotLabel(factoryTableName(i), "Table", i));
    return v;
}

std::vector<std::string> waveLabels()
{
    std::vector<std::string> v;
    for (int i = 0; i < kNumWaves; ++i) v.push_back(slotLabel(factoryWaveName(i), "Wave", i));
    return v;
}

std::vector<std::string> cmdLabels()
{
    // Letter plus word, 8 characters or fewer. The table grid and the editor use the letter alone.
    static const char* const kWords[kNumCmdChoices] = {"-",       "D Delay", "E Env",   "H Hop",   "K Kill",  "L Slide",
                                                       "N Noise", "P Pitch", "R Retrig", "S Sweep", "V Vib",  "W Wave"};
    std::vector<std::string> v;
    for (int i = 0; i < kNumCmdChoices; ++i) v.push_back(kWords[i]);
    return v;
}

// Ranges shared by buildParams (the parameter bounds) and proposedSettings (the
// clamps), so each is stated once.
constexpr int kVolMax = 15, kEnvSpdMax = 7, kDutyMax = 3, kTspRange = 24, kFineRange = 50, kLenMax = 64;

// NOI FixPitch: every (shift 0..13, divisor code 0..7) NR43 setting, sorted by noise clock frequency
// 262144 / (r * 2^(s+1)) (r = 0.5 for code 0) ascending, equal frequencies collapsed to one entry (the
// one with the highest register index shift * 8 + code). A value v is the v-th lowest frequency, so a
// higher value is strictly a higher pitch. The encoder takes the register index (noiseFixedShape).
// r * 2^(s+1) = r2 * 2^s with r2 = 1 for code 0, else 2 * code: a smaller r2 * 2^s is a higher frequency.
struct FixPitchTable
{
    int n = 0;
    int regIndex[112] = {};   // value -> shift * 8 + code
    int period[112] = {};     // value -> r2 * 2^s (frequency = 262144 / period)
};

constexpr int fixPitchPeriod(int idx) { return (idx % 8 == 0 ? 1 : 2 * (idx % 8)) << (idx / 8); }

constexpr FixPitchTable makeFixPitchTable()
{
    FixPitchTable t;
    int idx[112] = {};
    for (int i = 0; i < 112; ++i) idx[i] = i;
    // Insertion sort: period descending (frequency ascending), then register index descending.
    for (int i = 1; i < 112; ++i)
    {
        const int v = idx[i];
        int j = i - 1;
        while (j >= 0 && (fixPitchPeriod(idx[j]) < fixPitchPeriod(v) ||
                          (fixPitchPeriod(idx[j]) == fixPitchPeriod(v) && idx[j] < v)))
        {
            idx[j + 1] = idx[j];
            --j;
        }
        idx[j + 1] = v;
    }
    for (int i = 0; i < 112; ++i)
    {
        if (t.n > 0 && t.period[t.n - 1] == fixPitchPeriod(idx[i])) continue;   // duplicate frequency
        t.regIndex[t.n] = idx[i];
        t.period[t.n] = fixPitchPeriod(idx[i]);
        ++t.n;
    }
    return t;
}

constexpr FixPitchTable kFixPitch = makeFixPitchTable();
constexpr int kFixPitchMax = kFixPitch.n - 1;

// The value whose frequency is closest (log scale) to that of register index `idx`.
constexpr int fixPitchValueNear(int idx)
{
    const int p = fixPitchPeriod(idx);
    int best = 0;
    double bestErr = 1e30;
    for (int v = 0; v < kFixPitch.n; ++v)
    {
        const double r = static_cast<double>(kFixPitch.period[v]) / p;
        const double err = r > 1.0 ? r : 1.0 / r;
        if (err < bestErr) { bestErr = err; best = v; }
    }
    return best;
}
constexpr int kNone = -1;

// Parameter index of each per-channel field (kNone = the channel has no such
// parameter; the ChannelSettings default stays).
struct ChannelParamIds
{
    int on, vol, envDir, envSpd, duty, tsp, fine, pan, len, tbl;
};
constexpr ChannelParamIds kChannelParams[4] = {
    {P1_On, P1_Vol, P1_EnvDir, P1_EnvSpd, P1_Duty, P1_Tsp, P1_Fine, P1_Pan, P1_Len, P1_Tbl},
    {P2_On, P2_Vol, P2_EnvDir, P2_EnvSpd, P2_Duty, P2_Tsp, P2_Fine, P2_Pan, P2_Len, P2_Tbl},
    {WV_On, kNone, kNone, kNone, kNone, WV_Tsp, WV_Fine, WV_Pan, WV_Len, WV_Tbl},
    {NZ_On, NZ_Vol, NZ_EnvDir, NZ_EnvSpd, kNone, NZ_Tsp, kNone, NZ_Pan, NZ_Len, NZ_Tbl}};

// The fields every channel shares.
void readChannel(const ChannelParamIds& id, const float* v, ChannelSettings& c)
{
    auto I = [&](int i) { return static_cast<int>(std::lround(v[i])); };
    c.enabled = I(id.on) != 0;
    if (id.vol != kNone) c.volume = static_cast<uint8_t>(std::clamp<int>(I(id.vol), 0, kVolMax));
    if (id.envDir != kNone) c.envUp = I(id.envDir) == 1;
    if (id.envSpd != kNone) c.envPeriod = static_cast<uint8_t>(std::clamp<int>(I(id.envSpd), 0, kEnvSpdMax));
    if (id.duty != kNone) c.duty = static_cast<uint8_t>(std::clamp<int>(I(id.duty), 0, kDutyMax));
    c.transpose = static_cast<int8_t>(std::clamp<int>(I(id.tsp), -kTspRange, kTspRange));
    if (id.fine != kNone) c.fineCents = static_cast<int8_t>(std::clamp<int>(I(id.fine), -kFineRange, kFineRange));
    c.pan = static_cast<Pan>(std::clamp<int>(I(id.pan), 0, 3));
    c.length = static_cast<uint8_t>(std::clamp<int>(I(id.len), 0, kLenMax));
    c.tableSlot = static_cast<int8_t>(std::clamp<int>(I(id.tbl), 0, 16) - 1);
}

}  // namespace

int cmdIndexFromByte(uint8_t b)
{
    for (int i = 1; i < kNumCmdChoices; ++i)
        if (static_cast<uint8_t>(kCmdLetters[i]) == b) return i;
    return 0;
}

uint8_t cmdByteFromIndex(int index)
{
    return static_cast<uint8_t>(kCmdLetters[std::clamp<int>(index, 0, kNumCmdChoices - 1)]);
}

float volumeToGain(float db) { return std::pow(10.0f, db / 20.0f); }

std::vector<mpc::Param> buildParams()
{
    std::vector<mpc::Param> p;
    p.reserve(kTotalParams);
    auto add = [&](mpc::Param q) { p.push_back(std::move(q)); };

    add(mpc::intParam("bendrng", "Bend Range", 0, 24, 2, "st"));
    add(mpc::floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
    add(mpc::choiceParam("retrig", "Retrigger", {"Always", "Legato"}, 0));
    add(mpc::choiceParam("noteoff", "Note Off", {"Cut", "Hold"}, 0));
    add(mpc::boolParam("velvol", "Vel to Level", false));
    add(mpc::choiceParam("tblmode", "Table Sync", {"Tempo", "Free"}, 0));
    {
        std::vector<std::string> rate;
        for (int i = 0; i < kNumTickChoices; ++i) rate.push_back(kTickChoiceLabels[i]);
        add(mpc::choiceParam("tblrate", "Table Rate", rate, kDefaultTickChoice));
    }
    add(mpc::choiceParam("tblphase", "Table Start", {"Note", "Grid"}, 0));
    add(mpc::floatParam("freehz", "Free Rate", 1.0f, 240.0f, static_cast<float>(kDmgFrameHz), "", 2));

    // Pulse 1 then Pulse 2.
    for (int c = 0; c < 2; ++c)
    {
        const std::string id = c == 0 ? "p1_" : "p2_";
        const std::string nm = c == 0 ? "PU1 " : "PU2 ";
        add(mpc::boolParam((id + "on").c_str(), (nm + "On").c_str(), c == 0));
        add(mpc::intParam((id + "vol").c_str(), (nm + "Level").c_str(), 0, kVolMax, 15));
        add(mpc::choiceParam((id + "envdir").c_str(), (nm + "Env Dir").c_str(), kEnvDirLabels, 0));
        add(mpc::intParam((id + "envspd").c_str(), (nm + "Env Time").c_str(), 0, kEnvSpdMax, 3));
        add(mpc::choiceParam((id + "duty").c_str(), (nm + "Duty").c_str(), kDutyLabels, c == 0 ? 2 : 1));
        add(mpc::intParam((id + "tsp").c_str(), (nm + "Semi").c_str(), -kTspRange, kTspRange, 0));
        add(mpc::intParam((id + "fine").c_str(), (nm + "Fine").c_str(), -kFineRange, kFineRange, 0));
        add(mpc::choiceParam((id + "pan").c_str(), (nm + "Pan").c_str(), kPanLabels, 2));
        add(mpc::intParam((id + "len").c_str(), (nm + "Length").c_str(), 0, kLenMax, 0));
        if (c == 0)
        {
            add(mpc::intParam("p1_swtime", "PU1 Swp Time", 0, 7, 0));
            // Default Up keeps the engine default (sweepDown false) after the choice order change.
            add(mpc::choiceParam("p1_swdir", "PU1 Swp Dir", kSwpDirLabels, 1));
            add(mpc::intParam("p1_swshift", "PU1 Swp Amt", 0, 7, 0));
        }
        add(mpc::choiceParam((id + "tbl").c_str(), (nm + "Table").c_str(), tableLabels(), 0));
    }

    // Wave
    add(mpc::boolParam("wv_on", "WAV On", false));
    add(mpc::choiceParam("wv_level", "WAV Level", kWaveLevelLabels, 3));
    add(mpc::choiceParam("wv_wave", "WAV Shape", waveLabels(), 0));
    add(mpc::intParam("wv_tsp", "WAV Semi", -kTspRange, kTspRange, -12));
    add(mpc::intParam("wv_fine", "WAV Fine", -kFineRange, kFineRange, 0));
    add(mpc::choiceParam("wv_pan", "WAV Pan", kPanLabels, 2));
    add(mpc::intParam("wv_len", "WAV Length", 0, kLenMax, 0));
    add(mpc::choiceParam("wv_tbl", "WAV Table", tableLabels(), 0));

    // Noise
    add(mpc::boolParam("nz_on", "NOI On", false));
    add(mpc::intParam("nz_vol", "NOI Level", 0, kVolMax, 15));
    add(mpc::choiceParam("nz_envdir", "NOI Env Dir", kEnvDirLabels, 0));
    add(mpc::intParam("nz_envspd", "NOI Env Time", 0, kEnvSpdMax, 2));
    add(mpc::choiceParam("nz_width", "NOI Mode", {"Hiss 15", "Buzz 7"}, 0));
    add(mpc::choiceParam("nz_pmode", "NOI Pitch", {"Track", "Fixed"}, 0));
    // FixPitch: pitch-sorted (see kFixPitch); the default is the old default register index 40
    // (shift 5, code 0, 8192 Hz).
    add(mpc::intParam("nz_shape", "NOI FixPitch", 0, kFixPitchMax, fixPitchValueNear(40)));
    add(mpc::intParam("nz_tsp", "NOI Semi", -kTspRange, kTspRange, 0));
    add(mpc::choiceParam("nz_pan", "NOI Pan", kPanLabels, 2));
    add(mpc::intParam("nz_len", "NOI Length", 0, kLenMax, 0));
    add(mpc::choiceParam("nz_tbl", "NOI Table", tableLabels(), 0));

    // Editor (51..58). Non-automatable. Defaults are placeholders: the plugin
    // resyncs them from the cell under the cursor.
    add(mpc::intParam("ed_tbl", "Edit Table", 1, 16, 1));
    add(mpc::intParam("ed_row", "Edit Row", 0, 15, 0));
    add(mpc::intParam("ed_vol", "Row Vol", -1, 255, -1));
    add(mpc::intParam("ed_tsp", "Row Semi", -48, 48, 0));
    add(mpc::choiceParam("ed_cmd1", "Row Cmd1", cmdLabels(), 0));
    add(mpc::intParam("ed_val1", "Row Val1", 0, 255, 0));
    add(mpc::choiceParam("ed_cmd2", "Row Cmd2", cmdLabels(), 0));
    add(mpc::intParam("ed_val2", "Row Val2", 0, 255, 0));

    // .sav import (59..62). The file index counts into the last scan of
    // Content/Import (at most 64 files); its text is the file name (the plugin
    // formats it). The three triggers reset to Off after they act.
    add(mpc::intParam("imp_file", "Import File", 1, 64, 1));
    add(mpc::boolParam("imp_go", "Import Now", false));
    add(mpc::boolParam("imp_undo", "Import Undo", false));
    add(mpc::boolParam("imp_rescan", "Import Scan", false));
    for (int i = kFirstEditorParam; i < kNumParams; ++i) p[static_cast<size_t>(i)].automatable = false;

    CpuProbe::appendParam(p);   // probe builds only
    return p;
}

int noiseFixPitchCount() { return kFixPitch.n; }
int noiseFixPitchRegIndex(int value) { return kFixPitch.regIndex[std::clamp(value, 0, kFixPitchMax)]; }
double noiseFixPitchHz(int value) { return 262144.0 / kFixPitch.period[std::clamp(value, 0, kFixPitchMax)]; }
int noiseFixPitchValueNear(int regIndex) { return fixPitchValueNear(regIndex); }

bool formatParamValue(int index, float plain, std::string& out)
{
    char buf[16];
    const int iv = static_cast<int>(std::lround(plain));
    switch (index)
    {
        case P1_Len: case P2_Len: case NZ_Len: case WV_Len:
        {
            // Length counts at 256 Hz: a Pulse / Noise step is 1/256 s (1..64: 4..250 ms), a Wave step is
            // 4/256 s (1..64: 16..1000 ms). Rounded to whole milliseconds.
            if (iv <= 0) { out = "Off"; return true; }
            const int n = std::min(iv, kLenMax);
            const int ms = ((index == WV_Len ? 4000 : 1000) * n + 128) / 256;
            std::snprintf(buf, sizeof(buf), "%dms", ms);
            out = buf;
            return true;
        }
        case P1_EnvSpd: case P2_EnvSpd: case NZ_EnvSpd: case P1_SwTime: case P1_SwShift:
            if (iv <= 0) { out = "Off"; return true; }
            std::snprintf(buf, sizeof(buf), "%d", iv);
            out = buf;
            return true;
        case P1_Tsp: case P2_Tsp: case WV_Tsp: case NZ_Tsp: case ED_Tsp:
            std::snprintf(buf, sizeof(buf), iv == 0 ? "%d st" : "%+d st", iv);
            out = buf;
            return true;
        case P1_Fine: case P2_Fine: case WV_Fine:
            std::snprintf(buf, sizeof(buf), iv == 0 ? "%d ct" : "%+d ct", iv);
            out = buf;
            return true;
        case P_FreeHz:
            std::snprintf(buf, sizeof(buf), "%.2fHz", static_cast<double>(plain));
            out = buf;
            return true;
        case ED_Row:
            // Two-digit hex, as LSDJ and the TABLE grid number rows.
            std::snprintf(buf, sizeof(buf), "%02X", std::clamp<int>(iv, 0, 15));
            out = buf;
            return true;
        case ED_Vol:
        {
            const int v = iv;
            if (v < 0) { out = "Keep"; return true; }
            std::snprintf(buf, sizeof(buf), "%02X", std::clamp<int>(v, 0, 255));
            out = buf;
            return true;
        }
        case ED_Val1: case ED_Val2:
            std::snprintf(buf, sizeof(buf), "%02X", std::clamp<int>(static_cast<int>(std::lround(plain)), 0, 255));
            out = buf;
            return true;
        default:
            return false;
    }
}

EngineSettings proposedSettings(const float* v)
{
    auto I = [&](int i) { return static_cast<int>(std::lround(v[i])); };
    EngineSettings s;
    s.bendRange = static_cast<uint8_t>(std::clamp<int>(I(P_BendRange), 0, 24));
    s.retrigger = I(P_Retrigger) == 1 ? RetriggerMode::Legato : RetriggerMode::Always;
    s.noteOff = I(P_NoteOff) == 1 ? NoteOffMode::Ring : NoteOffMode::Cut;
    s.velToVol = I(P_VelVol) != 0;
    s.tick.enabled = true;
    s.tick.mode = I(P_TblClock) == 1 ? TickMode::Free : TickMode::Sync;
    s.tick.ticksPerBeat = static_cast<uint16_t>(kTicksPerBeatChoices[std::clamp<int>(I(P_TblRate), 0, kNumTickChoices - 1)]);
    s.tick.phase = I(P_TblPhase) == 1 ? TickPhase::Grid : TickPhase::Note;
    s.tick.freeHz = v[P_FreeHz];

    for (int c = 0; c < 4; ++c) readChannel(kChannelParams[c], v, s.ch[c]);

    // Channel-specific fields.
    {
        ChannelSettings& c = s.ch[0];   // Pulse 1 sweep
        c.sweepTime = static_cast<uint8_t>(std::clamp<int>(I(P1_SwTime), 0, 7));
        c.sweepDown = I(P1_SwDir) == 0;   // choices: Down, Up
        // Swp Amt: Off, 1..7 with a higher value a bigger pitch change; the register shift is 8 - amt
        // (a smaller shift is a bigger step). Off keeps shift 0.
        const int amt = std::clamp<int>(I(P1_SwShift), 0, 7);
        c.sweepShift = static_cast<uint8_t>(amt == 0 ? 0 : 8 - amt);
    }
    {
        ChannelSettings& c = s.ch[2];   // Wave
        c.waveLevel = static_cast<WaveLevel>(std::clamp<int>(I(WV_Level), 0, 3));
        c.waveSlot = static_cast<uint8_t>(std::clamp<int>(I(WV_Wave), 0, 15));
    }
    {
        ChannelSettings& c = s.ch[3];   // Noise
        c.noiseWidth7 = I(NZ_Width) == 1;
        c.noisePitchMode = I(NZ_PitchMode) == 1 ? NoisePitchMode::Fixed : NoisePitchMode::Track;
        c.noiseFixedShape = static_cast<uint8_t>(kFixPitch.regIndex[std::clamp<int>(I(NZ_Shape), 0, kFixPitchMax)]);
    }
    return s;
}

// ---- programs --------------------------------------------------------------

namespace {

// Starts from every parameter's default (indices 0..50) and applies overrides.
struct ProgBuilder
{
    std::vector<float> v;
    ProgBuilder()
    {
        const std::vector<mpc::Param> p = buildParams();
        for (int i = 0; i < kNumEngineParams; ++i) v.push_back(p[static_cast<size_t>(i)].def);
    }
    ProgBuilder& set(int index, float plain) { v[static_cast<size_t>(index)] = plain; return *this; }
    // Table choices: 0 = Off, 1..16 = table number (1 MajArp, 2 MinArp, 3 Octave,
    // 4 Vibrato, 5 KickDrop, 6 Fade, 7 Rise, 8 Hat). Wave choices are 0-based
    // (0 = wave 1: Saw, 1 Square, 2 Triangle, 3 Sine, 4 Pulse25, 5 Pulse12, 6 HalfSine, 7 Organ).
};

std::vector<ProgramDef> makePrograms()
{
    std::vector<ProgramDef> out;
    auto add = [&](const char* name, const ProgBuilder& b) { out.push_back({name, b.v}); };

    // Init: Pulse 1 only, parameter defaults.
    add("Init", ProgBuilder());

    // ChipLead: sustained 50 % pulse with the Vibrato table.
    {
        ProgBuilder b;
        b.set(P1_EnvSpd, 0).set(P1_Duty, 2).set(P1_Tbl, 4);
        add("ChipLead", b);
    }
    // MajArp: 25 % pulse running the MajArp table (+0 +4 +3 -7).
    {
        ProgBuilder b;
        b.set(P1_EnvSpd, 0).set(P1_Duty, 1).set(P1_Tbl, 1);
        add("MajArp", b);
    }
    // WaveBass: Wave channel only, triangle, one octave down.
    {
        ProgBuilder b;
        b.set(P1_On, 0).set(WV_On, 1).set(WV_Wave, 2).set(WV_Level, 3).set(WV_Tsp, -12);
        add("WaveBass", b);
    }
    // Kick: Pulse 1 with the KickDrop table (-5 semitones per tick, then cut).
    {
        ProgBuilder b;
        b.set(P1_Duty, 2).set(P1_EnvSpd, 4).set(P1_Tsp, -12).set(P1_Tbl, 5);
        add("Kick", b);
    }
    // Hat: Noise only, 7-bit, fixed pitch, short decay, Hat table.
    {
        ProgBuilder b;
        b.set(P1_On, 0).set(NZ_On, 1).set(NZ_Width, 1).set(NZ_PitchMode, 1).set(NZ_Shape, fixPitchValueNear(12))
            .set(NZ_EnvSpd, 1).set(NZ_Tbl, 8).set(P_VelVol, 1);
        add("Hat", b);
    }
    // Stack4: all four channels, each with a different factory table.
    {
        ProgBuilder b;
        b.set(P1_On, 1).set(P1_EnvSpd, 0).set(P1_Vol, 12).set(P1_Duty, 2).set(P1_Tbl, 1)
            .set(P2_On, 1).set(P2_EnvSpd, 0).set(P2_Vol, 9).set(P2_Duty, 1).set(P2_Tsp, 12).set(P2_Tbl, 2)
            .set(WV_On, 1).set(WV_Wave, 2).set(WV_Level, 3).set(WV_Tsp, -12).set(WV_Tbl, 3)
            .set(NZ_On, 1).set(NZ_Vol, 8).set(NZ_EnvSpd, 3).set(NZ_Tbl, 5);
        add("Stack4", b);
    }
    // Zap: 12.5 % pulse with a fast downward sweep, an octave up.
    {
        ProgBuilder b;
        b.set(P1_Duty, 0).set(P1_EnvSpd, 2).set(P1_Tsp, 12).set(P1_SwTime, 3).set(P1_SwDir, 0).set(P1_SwShift, 4);
        add("Zap", b);
    }
    return out;
}

}  // namespace

const std::vector<ProgramDef>& programDefs()
{
    static const std::vector<ProgramDef> defs = makePrograms();
    return defs;
}

}  // namespace mpcboy
