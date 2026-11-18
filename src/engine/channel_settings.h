// src/engine/channel_settings.h - per-channel and engine-wide settings (POD).
// WP4, see .agents/plans/mpcboy-gb-synth/02-apu-and-voice.md.
#pragma once

#include <cstdint>

#include "engine_consts.h"

namespace mpcboy {

enum class Pan : uint8_t { Off = 0, L = 1, C = 2, R = 3 };

enum class NoisePitchMode : uint8_t { Track = 0, Fixed = 1 };

enum class RetriggerMode : uint8_t { Always = 0, Legato = 1 };

// Ring is shown as "Hold" ("Note Off": Cut / Hold).
enum class NoteOffMode : uint8_t { Cut = 0, Ring = 1 };

// Wave output level as shown to the user (NR32 bits 6-5 codes are not
// monotonic: 100 % = 01, 50 % = 10, 25 % = 11, mute = 00).
enum class WaveLevel : uint8_t { Mute = 0, P25 = 1, P50 = 2, P100 = 3 };

// Table tick clock (WP6, decision D7).
enum class TickMode : uint8_t { Sync = 0, Free = 1 };
enum class TickPhase : uint8_t { Note = 0, Grid = 1 };

// Ticks per quarter note offered on screen, in the order of the parameter
// choices. 24/beat is LSDJ's tick at its default groove (6 ticks per 1/16).
constexpr int kNumTickChoices = 10;
constexpr int kTicksPerBeatChoices[kNumTickChoices] = {96, 48, 32, 24, 16, 12, 8, 6, 4, 3};
constexpr const char* kTickChoiceLabels[kNumTickChoices] = {
    "96/beat", "48/beat", "32/beat", "24/beat", "16/beat",
    "12/beat", "8/beat",  "6/beat",  "4/beat",  "3/beat"};
constexpr int kDefaultTickChoice = 3;   // 24/beat (LSDJ timing; the value text does not say so)

struct TickSettings
{
    // false: the Engine generates no ticks itself; only EngineBlock::tickOffsets
    // drive the tables (test rigs, the CPU probe's legacy path).
    bool enabled = true;
    TickMode mode = TickMode::Sync;
    TickPhase phase = TickPhase::Note;
    uint16_t ticksPerBeat = 24;
    float freeHz = static_cast<float>(kDmgFrameHz);   // DMG frame rate
};

struct ChannelSettings
{
    bool enabled = true;
    uint8_t volume = 15;        // 0..15 initial envelope volume
    bool envUp = false;         // envelope direction (true = increase)
    uint8_t envPeriod = 0;      // 0..7 (0 = envelope off)
    uint8_t duty = 2;           // 0..3 (12.5 / 25 / 50 / 75 %), pulse only
    int8_t transpose = 0;       // -24..+24 semitones
    int8_t fineCents = 0;       // -50..+50 cents
    Pan pan = Pan::C;
    uint8_t length = 0;         // 0 = off, 1..64 steps (pulse/noise 1/256 s, wave 4/256 s)
    // Pulse 1 sweep (NR10)
    uint8_t sweepTime = 0;      // 0..7 (0 = sweep off)
    bool sweepDown = false;
    uint8_t sweepShift = 0;     // 0..7
    // Wave
    WaveLevel waveLevel = WaveLevel::P100;
    uint8_t waveSlot = 0;       // 0..15
    // Noise
    bool noiseWidth7 = false;   // 7-bit LFSR (NR43 bit 3)
    NoisePitchMode noisePitchMode = NoisePitchMode::Track;
    uint8_t noiseFixedShape = 0;   // 0..111: shift = idx / 8 (0..13), divisor code = idx % 8 (register order: a higher idx
                                   // is a lower pitch; the NOI FixPitch parameter is 111 - idx)
    // Table
    int8_t tableSlot = -1;      // -1 = off, 0..15
};

struct EngineSettings
{
    ChannelSettings ch[4];      // Pulse1, Pulse2, Wave, Noise
    uint8_t bendRange = 2;      // semitones for full pitch-bend deflection
    RetriggerMode retrigger = RetriggerMode::Always;
    NoteOffMode noteOff = NoteOffMode::Cut;
    bool velToVol = false;      // scale the initial envelope volume by velocity
    TickSettings tick;

    EngineSettings()
    {
        ch[0].duty = 2;
        ch[1].duty = 2;
    }
};

}  // namespace mpcboy
