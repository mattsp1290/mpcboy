// rackboy_core/rackboy_pitch.h
//
// Semitone-from-base-note to Game Boy APU pitch register mapping.
//
// This is the pitch side of the rackboy_core voice machine (bead
// rackboy-7fk). VoiceState::target_pitch (see rackboy_voice.h) is a signed
// semitone offset from the binding base note C5 / MIDI 72, as pinned in
// docs/prompts/rackboy-lsdj-quad-voice-module.md Architecture Decision #5
// ("C5 / MIDI 72 is the binding base note"). On each Gate 1 trigger, and
// on every table row that writes a pitch-affecting command, the table
// interpreter converts that semitone offset into the channel-specific
// register value using the helpers in this header, then writes that value
// to the corresponding NRx3/NRx4 pair.
//
// Three formulas are in play, all derived from the GB Pan Docs audio
// chapter (https://gbdev.io/pandocs/Audio.html):
//
//   * CH1 / CH2 (square):   freq_hz = 131072 / (2048 - x),  x in [0, 2047]
//   * CH3       (wave):     freq_hz =  65536 / (2048 - x),  x in [0, 2047]
//   * CH4       (noise):    freq_hz = 524288 / (r * 2^(s+1))
//                          with r from a divisor-code lookup
//                          ({8,16,32,48,64,80,96,112}) and s in [0, 13].
//                          (Algebraically identical to the alternate
//                          Pan Docs form `262144 / (r * 2^s)` — both
//                          describe the same hardware. Exact-byte tests
//                          in pitch_test.cpp lock the formula.)
//
// Each helper clamps its output to the valid hardware register range so
// an out-of-range semitone offset pins to the extreme pitch rather than
// wrapping or producing garbage bytes. No allocations, no exceptions,
// no STL in the audio-thread path — `noexcept` everywhere.

#ifndef RACKBOY_CORE_RACKBOY_PITCH_H_
#define RACKBOY_CORE_RACKBOY_PITCH_H_

#include <cstdint>

namespace rackboy {
namespace core {

// Binding base note for v1 per docs/prompts Architecture Decision #5.
// C5 is MIDI note 72 (middle C is C4 / MIDI 60 in the scientific pitch
// notation rackboy uses — the LSDJ community also calls this "C5" per
// its one-octave-up convention, but the normative note-number is 72).
constexpr std::int16_t kBaseMidiNote = 72;

// Upper bound on the 11-bit pitch registers shared by CH1 / CH2 / CH3
// (NRx3 + low 3 bits of NRx4). Exposed in the header so callers can
// sanity-check computed register values without re-deriving the mask.
constexpr std::uint16_t kSquareWaveFreqMax = 2047;

// ---------------------------------------------------------------------------
// CH1 / CH2 (square) — NR13/NR14 and NR23/NR24
// ---------------------------------------------------------------------------
// Given a semitone offset from the base note (positive = above C5), return
// the 11-bit frequency register value that produces the closest audible
// pitch. The result fits in 11 bits and is always in [0, kSquareWaveFreqMax].
// Callers split it across NRx3 (low 8 bits) and NRx4 (high 3 bits), with
// the remaining NRx4 bits carrying the length-enable / trigger flags from
// elsewhere. Extreme semitone values clamp to the endpoints of the
// hardware-representable range rather than wrapping.
std::uint16_t pitch_square_11bit(std::int16_t semitones_from_base) noexcept;

// ---------------------------------------------------------------------------
// CH3 (wave) — NR33/NR34
// ---------------------------------------------------------------------------
// Same shape as pitch_square_11bit but uses the CH3-specific Pan Docs
// divisor (65536 instead of 131072) because CH3's period counter advances
// a 32-sample wave RAM pointer rather than a single square-wave half-period.
// For the same input semitones, CH3's register value is NOT the same as
// CH1/CH2's — it lives one "octave lower" on the numeric scale, which is
// a useful sanity check in tests.
std::uint16_t pitch_wave_11bit(std::int16_t semitones_from_base) noexcept;

// ---------------------------------------------------------------------------
// CH4 (noise) — NR43
// ---------------------------------------------------------------------------
// Pick the (shift, divisor_code) pair whose Pan Docs-derived LFSR clock
// rate is closest to the target audible frequency implied by the semitone
// offset, then pack it into an NR43 byte (upper nibble = shift, bit 3 = 0
// for long-period LFSR, low 3 bits = divisor code). The returned byte is
// intentionally long-period (bit 3 clear) — LSDJ's "noise shape" command
// is the authoritative path for short-period (`N`) variants and is
// tracked separately in docs/LSDJ_REFERENCE.md. Shifts 14 and 15 are
// excluded because they freeze the LFSR per Pan Docs.
//
// Extreme semitones clamp to the representable NR43 range rather than
// wrapping.
//
// NOTE: LSDJ itself maintains a hand-tuned note→NR43 lookup that may
// differ slightly from this Pan-Docs-derived "closest audible match"
// search. A later bead may swap this implementation for the LSDJ table
// once the reference data is captured in docs/LSDJ_REFERENCE.md; the
// header contract is the MIDI semitone offset, not the exact byte, so
// that swap will not break callers. Tests in tests/core/pitch_test.cpp
// pin the current formula's outputs at a handful of reference notes so
// an accidental formula drift is caught loudly.
std::uint8_t pitch_noise_nr43(std::int16_t semitones_from_base) noexcept;

// MPCBOY LOCAL MOD: cents-resolution pitch. `cents_from_base` is the distance
// from kBaseMidiNote in cents (semitone * 100); the result is the nearest
// 11-bit register value, clamped like the semitone functions.
// pitch_square_11bit_cents(n * 100) == pitch_square_11bit(n), same for wave.
std::uint16_t pitch_square_11bit_cents(std::int32_t cents_from_base) noexcept;

std::uint16_t pitch_wave_11bit_cents(std::int32_t cents_from_base) noexcept;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_PITCH_H_
