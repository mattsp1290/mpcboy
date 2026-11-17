// rackboy_core/rackboy_pitch.cpp
//
// Implementation of the semitone-to-NRx3/NRx4 and semitone-to-NR43 helpers
// declared in rackboy_pitch.h. All formulas are sourced from the GB Pan
// Docs audio chapter (https://gbdev.io/pandocs/Audio.html), with the
// register-split conventions documented in the header.
//
// The implementation stays deliberately simple:
//   * No allocations.
//   * No exceptions (std::pow / std::lround set errno but do not throw).
//   * No STL containers.
//   * No persistent state — every call is pure.
//
// The noise helper uses a small brute-force search over the 14 shifts x
// 8 divisors = 112 candidate NR43 bytes and picks the one whose
// Pan-Docs-derived frequency is closest to the target. This is fast
// enough for the audio thread (we only call it on trigger + table edges,
// not per-sample), and a constexpr precomputed table can replace it later
// without changing the header contract.

#include "rackboy_pitch.h"

#include <cmath>

namespace rackboy {
namespace core {

namespace {

// Concert-A / A4 reference: MIDI note 69 == 440 Hz. Used to convert a
// MIDI note number to a frequency in Hz via the standard equal-tempered
// formula freq = 440 * 2^((midi - 69) / 12).
constexpr double kA4Hz = 440.0;
constexpr std::int16_t kA4MidiNote = 69;

// Pan Docs CH1/CH2 / CH3 period divisors. These are the numerators in
// the square-wave / wave-RAM frequency formulas.
constexpr double kSquareFreqNumerator = 131072.0;  // CH1 / CH2
constexpr double kWaveFreqNumerator   =  65536.0;  // CH3
constexpr double kPeriodCeiling       =   2048.0;  // 2048 - x in both

// Pan Docs NR43 (CH4) divisors indexed by the low 3-bit divisor code:
//   code 0 -> 8, code 1 -> 16, code 2 -> 32, code 3 -> 48,
//   code 4 -> 64, code 5 -> 80, code 6 -> 96, code 7 -> 112
constexpr int kNoiseDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};

// Convert a semitone offset from the base note into an absolute frequency
// in Hz. Uses std::exp2 for portability and speed — constexpr is not an
// option because exp2 is not constexpr in C++17.
inline double semitone_to_hz(std::int16_t semitones_from_base) noexcept
{
    const int midi_note = static_cast<int>(kBaseMidiNote) +
                          static_cast<int>(semitones_from_base);
    return kA4Hz * std::exp2(static_cast<double>(midi_note - kA4MidiNote) / 12.0);
}

// Shared helper that maps a target frequency to the 11-bit pitch register
// value for either CH1/CH2 or CH3. The only difference between the two
// channels is the numerator in the Pan Docs formula.
// MPCBOY LOCAL MOD: same expression as semitone_to_hz, but with a fractional
// semitone count. For whole semitones every intermediate is exactly equal to
// the integer version, so both paths round to the same register value.
inline double cents_to_hz(std::int32_t cents_from_base) noexcept
{
    const double semis = static_cast<double>(cents_from_base) / 100.0;
    const double midi_note = static_cast<double>(kBaseMidiNote) + semis;
    return kA4Hz * std::exp2((midi_note - static_cast<double>(kA4MidiNote)) / 12.0);
}

inline std::uint16_t hz_to_11bit(double freq_hz, double numerator) noexcept
{
    if (freq_hz <= 0.0) {
        // Zero or negative Hz is not representable — clamp to bass end.
        // This guard is also the short-circuit for the extreme-underflow
        // case: a huge negative semitone offset makes std::exp2 underflow
        // to exactly 0.0, which would make the subsequent
        // `numerator / freq_hz` overflow to +inf and force
        // std::lround to hit its C11 "domain error, unspecified return"
        // path. Every mainstream libc saturates to LONG_MIN/LONG_MAX, but
        // we prefer to return a defined value here rather than rely on
        // that behavior.
        return 0;
    }
    const double period    = numerator / freq_hz;
    const double raw_x     = kPeriodCeiling - period;
    const long   rounded_x = std::lround(raw_x);

    if (rounded_x < 0) {
        return 0;
    }
    if (rounded_x > static_cast<long>(kSquareWaveFreqMax)) {
        return kSquareWaveFreqMax;
    }
    return static_cast<std::uint16_t>(rounded_x);
}

}  // namespace

std::uint16_t pitch_square_11bit(std::int16_t semitones_from_base) noexcept
{
    return hz_to_11bit(semitone_to_hz(semitones_from_base),
                       kSquareFreqNumerator);
}

std::uint16_t pitch_wave_11bit(std::int16_t semitones_from_base) noexcept
{
    return hz_to_11bit(semitone_to_hz(semitones_from_base),
                       kWaveFreqNumerator);
}

// MPCBOY LOCAL MOD
std::uint16_t pitch_square_11bit_cents(std::int32_t cents_from_base) noexcept
{
    return hz_to_11bit(cents_to_hz(cents_from_base), kSquareFreqNumerator);
}

// MPCBOY LOCAL MOD
std::uint16_t pitch_wave_11bit_cents(std::int32_t cents_from_base) noexcept
{
    return hz_to_11bit(cents_to_hz(cents_from_base), kWaveFreqNumerator);
}

std::uint8_t pitch_noise_nr43(std::int16_t semitones_from_base) noexcept
{
    const double target_hz = semitone_to_hz(semitones_from_base);

    // Pan Docs: f_LFSR = 524288 / (divisor * 2^(shift + 1)). Shifts 14
    // and 15 freeze the LFSR, so we skip them — the v1 spec says the
    // noise channel is always audible, and a frozen LFSR would betray
    // that contract.
    //
    // Formula note: some older / third-party GB references give
    // `262144 / (divisor * 2^shift)` instead. That is the same function
    // — 524288 / (d * 2^(s+1)) == 262144 / (d * 2^s) algebraically —
    // rewritten so the numerator matches the APU's 524288 Hz master
    // oscillator directly. rackboy uses the canonical Pan Docs form
    // (https://gbdev.io/pandocs/Audio.html) and the exact-byte reference
    // tests in pitch_test.cpp will catch any accidental numerator swap.
    //
    // Scoring: we pick the candidate that minimises |log2(f / target)|,
    // i.e. the closest match in *pitch* (equal-ratio) space rather than
    // absolute Hz. On a coarse grid like NR43's the two metrics usually
    // agree, but Hz distance biases toward low-frequency candidates when
    // two pairs are symmetric around the target in ratio terms — which
    // is the wrong choice for a note-picker.
    constexpr int kMaxShift = 13;

    // Guard against a pathological target_hz of 0 (no sensible log2).
    if (target_hz <= 0.0) {
        return 0;
    }
    const double log2_target = std::log2(target_hz);

    std::uint8_t  best_byte  = 0;
    double        best_error = -1.0;  // sentinel: first candidate always wins

    for (int shift = 0; shift <= kMaxShift; ++shift) {
        const double two_to_shift_plus_one = std::exp2(static_cast<double>(shift + 1));
        for (int div_code = 0; div_code < 8; ++div_code) {
            const double divisor = static_cast<double>(kNoiseDivisors[div_code]);
            const double f_hz    = 524288.0 / (divisor * two_to_shift_plus_one);
            const double error   = std::abs(std::log2(f_hz) - log2_target);

            if (best_error < 0.0 || error < best_error) {
                best_error = error;
                // NR43 layout: bits 7..4 = shift, bit 3 = 0 (long-period LFSR),
                // bits 2..0 = divisor code.
                best_byte = static_cast<std::uint8_t>((shift << 4) | div_code);
            }
        }
    }

    return best_byte;
}

}  // namespace core
}  // namespace rackboy
