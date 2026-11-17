#include "param_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mpcboy {

namespace {

const std::vector<std::string> kDutyLabels = {"12.5%", "25%", "50%", "75%"};
const std::vector<std::string> kPanLabels = {"Off", "L", "C", "R"};
const std::vector<std::string> kWaveLevelLabels = {"0%", "25%", "50%", "100%"};
const std::vector<std::string> kEnvDirLabels = {"Down", "Up"};

std::vector<std::string> tableLabels()
{
    std::vector<std::string> v = {"Off"};
    for (int i = 1; i <= 16; ++i) v.push_back(std::to_string(i));
    return v;
}

std::vector<std::string> waveLabels()
{
    std::vector<std::string> v;
    for (int i = 1; i <= 16; ++i) v.push_back(std::to_string(i));
    return v;
}

std::vector<std::string> cmdLabels()
{
    std::vector<std::string> v;
    for (int i = 0; i < kNumCmdChoices; ++i) v.push_back(i == 0 ? "-" : std::string(1, kCmdLetters[i]));
    return v;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

int cmdIndexFromByte(uint8_t b)
{
    for (int i = 1; i < kNumCmdChoices; ++i)
        if (static_cast<uint8_t>(kCmdLetters[i]) == b) return i;
    return 0;
}

uint8_t cmdByteFromIndex(int index)
{
    return static_cast<uint8_t>(kCmdLetters[clampi(index, 0, kNumCmdChoices - 1)]);
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
    add(mpc::choiceParam("noteoff", "Note Off", {"Cut", "Ring"}, 0));
    add(mpc::boolParam("velvol", "Vel>Vol", false));
    add(mpc::choiceParam("tblmode", "Tbl Clock", {"Sync", "Free"}, 0));
    {
        std::vector<std::string> rate;
        for (int i = 0; i < kNumTickChoices; ++i) rate.push_back(kTickChoiceLabels[i]);
        add(mpc::choiceParam("tblrate", "Tbl Rate", rate, kDefaultTickChoice));
    }
    add(mpc::choiceParam("tblphase", "Tbl Phase", {"Note", "Grid"}, 0));
    add(mpc::floatParam("freehz", "Free Hz", 1.0f, 240.0f, 59.73f, "Hz", 2));

    // Pulse 1 then Pulse 2.
    for (int c = 0; c < 2; ++c)
    {
        const std::string id = c == 0 ? "p1_" : "p2_";
        const std::string nm = c == 0 ? "P1 " : "P2 ";
        add(mpc::boolParam((id + "on").c_str(), (nm + "On").c_str(), c == 0));
        add(mpc::intParam((id + "vol").c_str(), (nm + "Volume").c_str(), 0, 15, 15));
        add(mpc::choiceParam((id + "envdir").c_str(), (nm + "Env Dir").c_str(), kEnvDirLabels, 0));
        add(mpc::intParam((id + "envspd").c_str(), (nm + "Env Spd").c_str(), 0, 7, 3));
        add(mpc::choiceParam((id + "duty").c_str(), (nm + "Duty").c_str(), kDutyLabels, c == 0 ? 2 : 1));
        add(mpc::intParam((id + "tsp").c_str(), (nm + "Transp").c_str(), -24, 24, 0, "st"));
        add(mpc::intParam((id + "fine").c_str(), (nm + "Fine").c_str(), -50, 50, 0, "ct"));
        add(mpc::choiceParam((id + "pan").c_str(), (nm + "Pan").c_str(), kPanLabels, 2));
        add(mpc::intParam((id + "len").c_str(), (nm + "Length").c_str(), 0, 64, 0));
        if (c == 0)
        {
            add(mpc::intParam("p1_swtime", "P1 SwpTime", 0, 7, 0));
            add(mpc::choiceParam("p1_swdir", "P1 SwpDir", {"Up", "Down"}, 0));
            add(mpc::intParam("p1_swshift", "P1 SwpShift", 0, 7, 0));
        }
        add(mpc::choiceParam((id + "tbl").c_str(), (nm + "Table").c_str(), tableLabels(), 0));
    }

    // Wave
    add(mpc::boolParam("wv_on", "WV On", false));
    add(mpc::choiceParam("wv_level", "WV Level", kWaveLevelLabels, 3));
    add(mpc::choiceParam("wv_wave", "WV Wave", waveLabels(), 0));
    add(mpc::intParam("wv_tsp", "WV Transp", -24, 24, -12, "st"));
    add(mpc::intParam("wv_fine", "WV Fine", -50, 50, 0, "ct"));
    add(mpc::choiceParam("wv_pan", "WV Pan", kPanLabels, 2));
    add(mpc::intParam("wv_len", "WV Length", 0, 64, 0));
    add(mpc::choiceParam("wv_tbl", "WV Table", tableLabels(), 0));

    // Noise
    add(mpc::boolParam("nz_on", "NZ On", false));
    add(mpc::intParam("nz_vol", "NZ Volume", 0, 15, 15));
    add(mpc::choiceParam("nz_envdir", "NZ Env Dir", kEnvDirLabels, 0));
    add(mpc::intParam("nz_envspd", "NZ Env Spd", 0, 7, 2));
    add(mpc::choiceParam("nz_width", "NZ Width", {"15-bit", "7-bit"}, 0));
    add(mpc::choiceParam("nz_pmode", "NZ PitchMd", {"Track", "Fixed"}, 0));
    add(mpc::intParam("nz_shape", "NZ Shape", 0, 111, 40));
    add(mpc::intParam("nz_tsp", "NZ Transp", -24, 24, 0, "st"));
    add(mpc::choiceParam("nz_pan", "NZ Pan", kPanLabels, 2));
    add(mpc::intParam("nz_len", "NZ Length", 0, 64, 0));
    add(mpc::choiceParam("nz_tbl", "NZ Table", tableLabels(), 0));

    // Editor (51..58). Non-automatable. Defaults are placeholders: the plugin
    // resyncs them from the cell under the cursor.
    add(mpc::intParam("ed_tbl", "Edit Table", 1, 16, 1));
    add(mpc::intParam("ed_row", "Edit Row", 0, 15, 0));
    add(mpc::intParam("ed_vol", "Row Vol", -1, 255, -1));
    add(mpc::intParam("ed_tsp", "Row Transp", -48, 48, 0, "st"));
    add(mpc::choiceParam("ed_cmd1", "Row Cmd1", cmdLabels(), 0));
    add(mpc::intParam("ed_val1", "Row Val1", 0, 255, 0));
    add(mpc::choiceParam("ed_cmd2", "Row Cmd2", cmdLabels(), 0));
    add(mpc::intParam("ed_val2", "Row Val2", 0, 255, 0));

    // .sav import (59..62). The file index counts into the last scan of
    // Content/Import (at most 64 files); its text is the file name (the plugin
    // formats it). The three triggers reset to Off after they act.
    add(mpc::intParam("imp_file", "Imp File", 1, 64, 1));
    add(mpc::boolParam("imp_go", "Imp Go", false));
    add(mpc::boolParam("imp_undo", "Imp Undo", false));
    add(mpc::boolParam("imp_rescan", "Imp Rescan", false));
    for (int i = kFirstEditorParam; i < kNumParams; ++i) p[static_cast<size_t>(i)].automatable = false;

#ifdef MPCBOY_CPU_PROBE
    {
        mpc::Param dbg = mpc::floatParam("dbg_us", "CPU us", 0.0f, 100000.0f, 0.0f, "us", 1);
        dbg.automatable = false;   // read-only: the text comes from formatValue
        add(dbg);
    }
#endif
    return p;
}

bool formatParamValue(int index, float plain, std::string& out)
{
    char buf[16];
    switch (index)
    {
        case P1_Len: case P2_Len: case WV_Len: case NZ_Len:
            if (std::lround(plain) == 0) { out = "Off"; return true; }
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(plain)));
            out = buf;
            return true;
        case ED_Vol:
        {
            const int v = static_cast<int>(std::lround(plain));
            if (v < 0) { out = "--"; return true; }
            std::snprintf(buf, sizeof(buf), "%02X", clampi(v, 0, 255));
            out = buf;
            return true;
        }
        case ED_Val1: case ED_Val2:
            std::snprintf(buf, sizeof(buf), "%02X", clampi(static_cast<int>(std::lround(plain)), 0, 255));
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
    s.bendRange = static_cast<uint8_t>(clampi(I(P_BendRange), 0, 24));
    s.retrigger = I(P_Retrigger) == 1 ? RetriggerMode::Legato : RetriggerMode::Always;
    s.noteOff = I(P_NoteOff) == 1 ? NoteOffMode::Ring : NoteOffMode::Cut;
    s.velToVol = I(P_VelVol) != 0;
    s.masterGain = 1.0f;
    s.tick.enabled = true;
    s.tick.mode = I(P_TblClock) == 1 ? TickMode::Free : TickMode::Sync;
    s.tick.ticksPerBeat = static_cast<uint16_t>(kTicksPerBeatChoices[clampi(I(P_TblRate), 0, kNumTickChoices - 1)]);
    s.tick.phase = I(P_TblPhase) == 1 ? TickPhase::Grid : TickPhase::Note;
    s.tick.freeHz = v[P_FreeHz];

    auto pan = [&](int i) { return static_cast<Pan>(clampi(I(i), 0, 3)); };
    auto tbl = [&](int i) { return static_cast<int8_t>(clampi(I(i), 0, 16) - 1); };

    // Pulse 1
    {
        ChannelSettings& c = s.ch[0];
        c.enabled = I(P1_On) != 0;
        c.volume = static_cast<uint8_t>(clampi(I(P1_Vol), 0, 15));
        c.envUp = I(P1_EnvDir) == 1;
        c.envPeriod = static_cast<uint8_t>(clampi(I(P1_EnvSpd), 0, 7));
        c.duty = static_cast<uint8_t>(clampi(I(P1_Duty), 0, 3));
        c.transpose = static_cast<int8_t>(clampi(I(P1_Tsp), -24, 24));
        c.fineCents = static_cast<int8_t>(clampi(I(P1_Fine), -50, 50));
        c.pan = pan(P1_Pan);
        c.length = static_cast<uint8_t>(clampi(I(P1_Len), 0, 64));
        c.sweepTime = static_cast<uint8_t>(clampi(I(P1_SwTime), 0, 7));
        c.sweepDown = I(P1_SwDir) == 1;
        c.sweepShift = static_cast<uint8_t>(clampi(I(P1_SwShift), 0, 7));
        c.tableSlot = tbl(P1_Tbl);
    }
    // Pulse 2
    {
        ChannelSettings& c = s.ch[1];
        c.enabled = I(P2_On) != 0;
        c.volume = static_cast<uint8_t>(clampi(I(P2_Vol), 0, 15));
        c.envUp = I(P2_EnvDir) == 1;
        c.envPeriod = static_cast<uint8_t>(clampi(I(P2_EnvSpd), 0, 7));
        c.duty = static_cast<uint8_t>(clampi(I(P2_Duty), 0, 3));
        c.transpose = static_cast<int8_t>(clampi(I(P2_Tsp), -24, 24));
        c.fineCents = static_cast<int8_t>(clampi(I(P2_Fine), -50, 50));
        c.pan = pan(P2_Pan);
        c.length = static_cast<uint8_t>(clampi(I(P2_Len), 0, 64));
        c.tableSlot = tbl(P2_Tbl);
    }
    // Wave
    {
        ChannelSettings& c = s.ch[2];
        c.enabled = I(WV_On) != 0;
        c.waveLevel = static_cast<WaveLevel>(clampi(I(WV_Level), 0, 3));
        c.waveSlot = static_cast<uint8_t>(clampi(I(WV_Wave), 0, 15));
        c.transpose = static_cast<int8_t>(clampi(I(WV_Tsp), -24, 24));
        c.fineCents = static_cast<int8_t>(clampi(I(WV_Fine), -50, 50));
        c.pan = pan(WV_Pan);
        c.length = static_cast<uint8_t>(clampi(I(WV_Len), 0, 64));
        c.tableSlot = tbl(WV_Tbl);
    }
    // Noise
    {
        ChannelSettings& c = s.ch[3];
        c.enabled = I(NZ_On) != 0;
        c.volume = static_cast<uint8_t>(clampi(I(NZ_Vol), 0, 15));
        c.envUp = I(NZ_EnvDir) == 1;
        c.envPeriod = static_cast<uint8_t>(clampi(I(NZ_EnvSpd), 0, 7));
        c.noiseWidth7 = I(NZ_Width) == 1;
        c.noisePitchMode = I(NZ_PitchMode) == 1 ? NoisePitchMode::Fixed : NoisePitchMode::Track;
        c.noiseFixedShape = static_cast<uint8_t>(clampi(I(NZ_Shape), 0, 111));
        c.transpose = static_cast<int8_t>(clampi(I(NZ_Tsp), -24, 24));
        c.pan = pan(NZ_Pan);
        c.length = static_cast<uint8_t>(clampi(I(NZ_Len), 0, 64));
        c.tableSlot = tbl(NZ_Tbl);
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
        b.set(P1_On, 0).set(NZ_On, 1).set(NZ_Width, 1).set(NZ_PitchMode, 1).set(NZ_Shape, 12)
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
        b.set(P1_Duty, 0).set(P1_EnvSpd, 2).set(P1_Tsp, 12).set(P1_SwTime, 3).set(P1_SwDir, 1).set(P1_SwShift, 4);
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
