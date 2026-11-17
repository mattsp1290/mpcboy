// src/engine/table_bank.h - the user-editable content: 16 tables and 16 waves.
// WP5 (03-tables-and-tempo-sync.md). Plain trivially-copyable data so it can be
// memcpy'd through the staging slot and the published snapshot.
//
// Slot numbering: the engine, the table data (`H`, `W` params) and the state
// format use 0..15; the screen shows 1..16.
#pragma once

#include <cstdint>
#include <type_traits>

#include "rackboy_sav_types.h"

namespace mpcboy {

constexpr int kNumTables = 16;
constexpr int kNumWaves = 16;
constexpr int kWaveBytes = 16;   // 32 packed 4-bit samples, rackboy_apu_write_wave_ram format

// LSDJ table commands the interpreter implements, in editor order (index 0 =
// none). The single definition: the editor parameters and the .sav import
// report both derive from it.
constexpr int kNumCmdChoices = 12;
constexpr char kCmdLetters[kNumCmdChoices] = {0, 'D', 'E', 'H', 'K', 'L', 'N', 'P', 'R', 'S', 'V', 'W'};
constexpr bool isSupportedCommand(uint8_t c)
{
    for (int i = 1; i < kNumCmdChoices; ++i)
        if (static_cast<uint8_t>(kCmdLetters[i]) == c) return true;
    return false;
}

struct TableBank
{
    rackboy::sav::RackboyTable tables[kNumTables];
};

struct WaveBank
{
    uint8_t waves[kNumWaves][kWaveBytes];
};

struct Content
{
    TableBank tables;
    WaveBank waves;
};

static_assert(std::is_trivially_copyable<Content>::value, "Content is memcpy'd across threads");
static_assert(std::is_standard_layout<Content>::value, "Content layout must be stable");

// 4-bit sample i (0..31) of a packed wave; the high nibble is sample 2j.
inline int waveSample(const uint8_t wave[kWaveBytes], int i)
{
    const uint8_t b = wave[i >> 1];
    return (i & 1) ? (b & 0x0F) : (b >> 4);
}

inline void packWave(uint8_t out[kWaveBytes], const uint8_t samples[32])
{
    for (int j = 0; j < kWaveBytes; ++j)
        out[j] = static_cast<uint8_t>(((samples[2 * j] & 0x0F) << 4) | (samples[2 * j + 1] & 0x0F));
}

// All tables reset (empty, occupied = false) and all waves zero.
void clearContent(Content& c);

}  // namespace mpcboy
