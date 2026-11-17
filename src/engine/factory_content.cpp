#include "factory_content.h"

#include <algorithm>
#include <cmath>

namespace mpcboy {

namespace {

using rackboy::sav::RackboyTable;

void setRow(RackboyTable& t, int r, int tr, char c1 = 0, int p1 = 0, char c2 = 0, int p2 = 0)
{
    auto& row = t.rows[r];
    row.transpose = static_cast<int8_t>(tr);
    row.cmd1 = static_cast<uint8_t>(c1);
    row.cmd1_param = static_cast<uint8_t>(p1);
    row.cmd2 = static_cast<uint8_t>(c2);
    row.cmd2_param = static_cast<uint8_t>(p2);
}

void setVol(RackboyTable& t, int r, int vol)
{
    t.rows[r].vol = static_cast<uint8_t>(vol);
    t.rows[r].has_vol = true;
}

const char* const kTableNames[kNumTables] = {"MajArp", "MinArp", "Octave", "Vibrato", "KickDrop", "Fade",
                                             "Rise", "Hat", "", "", "", "", "", "", "", ""};
const char* const kWaveNames[kNumWaves] = {"Saw", "Square", "Triangle", "Sine", "Pulse25", "Pulse12",
                                           "HalfSine", "Organ", "", "", "", "", "", "", "", ""};

void makeWave(uint8_t out[kWaveBytes], int which)
{
    uint8_t s[32];
    const double kTwoPi = 6.283185307179586;
    double organ[32];
    double lo = 1e9, hi = -1e9;
    if (which == 7)
    {
        for (int i = 0; i < 32; ++i)
        {
            const double th = kTwoPi * i / 32.0;
            organ[i] = std::sin(th) + 0.5 * std::sin(2.0 * th) + 0.33 * std::sin(3.0 * th);   // fundamental + octave + fifth
            lo = std::min(lo, organ[i]);
            hi = std::max(hi, organ[i]);
        }
    }
    for (int i = 0; i < 32; ++i)
    {
        int v = 0;
        switch (which)
        {
            case 0: v = i * 15 / 31; break;                                   // Saw
            case 1: v = i < 16 ? 15 : 0; break;                                      // Square
            case 2: v = i < 16 ? i : 31 - i; break;                                  // Triangle (0..15..0)
            case 3: v = static_cast<int>(std::lround(7.5 + 7.5 * std::sin(kTwoPi * i / 32.0))); break;   // Sine
            case 4: v = i < 8 ? 15 : 0; break;                                       // Pulse 25 %
            case 5: v = i < 4 ? 15 : 0; break;                                       // Pulse 12.5 %
            case 6: v = i < 16 ? static_cast<int>(std::lround(15.0 * std::sin(3.141592653589793 * i / 16.0))) : 0; break;   // half sine
            default: v = static_cast<int>(std::lround(15.0 * (organ[i] - lo) / (hi - lo))); break;   // Organ
        }
        s[i] = static_cast<uint8_t>(std::min(15, std::max(0, v)));
    }
    packWave(out, s);
}

}  // namespace

const char* factoryTableName(int slot) { return (slot >= 0 && slot < kNumTables) ? kTableNames[slot] : ""; }
const char* factoryWaveName(int slot) { return (slot >= 0 && slot < kNumWaves) ? kWaveNames[slot] : ""; }

void loadFactoryContent(Content& c)
{
    clearContent(c);
    RackboyTable* t = c.tables.tables;
    for (int i = 0; i < 8; ++i) t[i].occupied = true;

    // 1 MajArp: +0 +4 +3, then -7 and loop to row 1.
    setRow(t[0], 0, 0); setRow(t[0], 1, 4); setRow(t[0], 2, 3); setRow(t[0], 3, -7, 'H', 1);
    // 2 MinArp
    setRow(t[1], 0, 0); setRow(t[1], 1, 3); setRow(t[1], 2, 4); setRow(t[1], 3, -7, 'H', 1);
    // 3 Octave
    setRow(t[2], 0, 0); setRow(t[2], 1, 12); setRow(t[2], 2, -12, 'H', 1);
    // 4 Vibrato: V42 on row 0, then hold on row 1.
    setRow(t[3], 0, 0, 'V', 0x42); setRow(t[3], 1, 0, 'H', 1);
    // 5 KickDrop: -5 per tick on rows 1-5, kill on row 6.
    setRow(t[4], 0, 0);
    for (int r = 1; r <= 5; ++r) setRow(t[4], r, -5);
    setRow(t[4], 6, 0, 'K', 0);
    // 6 Fade: volume column F0, E0 ... 00 (high nibble), hold on the last row.
    for (int r = 0; r < 16; ++r) { setRow(t[5], r, 0); setVol(t[5], r, (15 - r) << 4); }
    setRow(t[5], 15, 0, 'H', 15);
    // 7 Rise: P02 on row 0 and a kill after 16 ticks.
    setRow(t[6], 0, 0, 'P', 0x02, 'K', 0x10);
    // 8 Hat (Noise): F0 on row 0, silent from row 2 on.
    setRow(t[7], 0, 0); setVol(t[7], 0, 0xF0);
    setRow(t[7], 2, 0, 'H', 2); setVol(t[7], 2, 0x00);

    for (int i = 0; i < 8; ++i) makeWave(c.waves.waves[i], i);
}

}  // namespace mpcboy
