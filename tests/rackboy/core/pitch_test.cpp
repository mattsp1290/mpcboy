// tests/core/pitch_test.cpp
//
// Unit tests for rackboy_core/rackboy_pitch.h. The reference values in
// this file are all derived from the GB Pan Docs audio chapter
// (https://gbdev.io/pandocs/Audio.html) combined with the equal-tempered
// frequency formula freq = 440 * 2^((midi - 69) / 12). Each exact-byte
// check includes the arithmetic in a comment so a failing assertion can
// be debugged without re-deriving the expected value from scratch.
//
// C5 / MIDI 72 is the binding base note per docs/prompts Architecture
// Decision #5, so all semitone inputs are relative to MIDI 72.

#include <cmath>

#include "doctest/doctest.h"

#include "rackboy_pitch.h"

using rackboy::core::kBaseMidiNote;
using rackboy::core::kSquareWaveFreqMax;
using rackboy::core::pitch_noise_nr43;
using rackboy::core::pitch_square_11bit;
using rackboy::core::pitch_wave_11bit;

TEST_CASE("kBaseMidiNote pins C5 / MIDI 72 per Architecture Decision #5")
{
    CHECK_EQ(kBaseMidiNote, std::int16_t{72});
}

TEST_CASE("pitch_square_11bit hits Pan-Docs reference values at well-known notes")
{
    // C5 = MIDI 72 = 523.2511... Hz
    //   x = 2048 - round(131072 / 523.2511) = 2048 - 250 = 1798
    CHECK_EQ(pitch_square_11bit(0), std::uint16_t{1798});

    // A4 = MIDI 69 = 440 Hz (three semitones below C5)
    //   x = 2048 - round(131072 / 440) = 2048 - 298 = 1750
    CHECK_EQ(pitch_square_11bit(-3), std::uint16_t{1750});

    // C4 = MIDI 60 = 261.6256 Hz (one octave below C5)
    //   x = 2048 - round(131072 / 261.6256) = 2048 - 501 = 1547
    CHECK_EQ(pitch_square_11bit(-12), std::uint16_t{1547});

    // C6 = MIDI 84 = 1046.5023 Hz (one octave above C5)
    //   x = 2048 - round(131072 / 1046.50) = 2048 - 125 = 1923
    CHECK_EQ(pitch_square_11bit(12), std::uint16_t{1923});

    // A5 = MIDI 81 = 880 Hz
    //   x = 2048 - round(131072 / 880) = 2048 - 149 = 1899
    CHECK_EQ(pitch_square_11bit(9), std::uint16_t{1899});
}

TEST_CASE("pitch_square_11bit clamps extreme semitones to the 11-bit range")
{
    // Absurdly high note — way above the GB APU's representable pitch.
    CHECK_EQ(pitch_square_11bit(120), kSquareWaveFreqMax);

    // Absurdly low note — clamps to 0 (bass end of the register).
    CHECK_EQ(pitch_square_11bit(-120), std::uint16_t{0});

    // Result always fits in 11 bits, across the whole ±96 semitone sweep.
    for (int s = -96; s <= 96; ++s) {
        const std::uint16_t x =
            pitch_square_11bit(static_cast<std::int16_t>(s));
        CAPTURE(s);
        CHECK_LE(x, kSquareWaveFreqMax);
    }
}

TEST_CASE("pitch_wave_11bit hits Pan-Docs reference values at well-known notes")
{
    // C5 on CH3:
    //   x = 2048 - round(65536 / 523.2511) = 2048 - 125 = 1923
    CHECK_EQ(pitch_wave_11bit(0), std::uint16_t{1923});

    // C4 on CH3:
    //   x = 2048 - round(65536 / 261.6256) = 2048 - 250 = 1798
    CHECK_EQ(pitch_wave_11bit(-12), std::uint16_t{1798});

    // C6 on CH3:
    //   x = 2048 - round(65536 / 1046.50) = 2048 - 63 = 1985
    CHECK_EQ(pitch_wave_11bit(12), std::uint16_t{1985});

    // A4 on CH3:
    //   x = 2048 - round(65536 / 440) = 2048 - 149 = 1899
    CHECK_EQ(pitch_wave_11bit(-3), std::uint16_t{1899});
}

TEST_CASE("CH3 register at semitone s equals CH1/CH2 register at s+12 "
          "(CH3 is one octave lower for the same register value)")
{
    // This is a Pan-Docs-level invariant: the CH3 formula numerator is
    // exactly half the CH1/CH2 numerator (65536 vs 131072), which
    // corresponds to a one-octave frequency shift for any given period
    // register value. Checking it across a wide sweep gives us a strong
    // cross-channel sanity signal without having to re-derive each
    // expected byte by hand.
    for (int s = -36; s <= 36; ++s) {
        const std::uint16_t wave_x   =
            pitch_wave_11bit(static_cast<std::int16_t>(s));
        const std::uint16_t square_x =
            pitch_square_11bit(static_cast<std::int16_t>(s + 12));
        CAPTURE(s);
        CHECK_EQ(wave_x, square_x);
    }
}

TEST_CASE("pitch_noise_nr43 packs the NR43 byte in the Pan-Docs layout")
{
    // Layout: bits 7..4 = shift, bit 3 = width (always 0 here,
    // long-period LFSR), bits 2..0 = divisor code.
    //
    // For every semitone in a broad sweep, bit 3 must be clear (we
    // advertise long-period LFSR in the header) and the shift nibble
    // must be <= 13 (14 and 15 freeze the LFSR).
    for (int s = -60; s <= 60; ++s) {
        const std::uint8_t byte =
            pitch_noise_nr43(static_cast<std::int16_t>(s));
        CAPTURE(s);
        CHECK_EQ(byte & 0x08, 0);          // width bit clear
        const int shift = (byte >> 4) & 0x0F;
        CHECK_LE(shift, 13);
    }
}

TEST_CASE("pitch_noise_nr43 hits exact reference NR43 bytes at well-known notes")
{
    // These reference bytes are derived from the Pan Docs NR43 formula
    // under the log2 (pitch-distance) search metric used by
    // pitch_noise_nr43. On the coarse NR43 grid the selection
    // algorithm always lands on the same integer product of
    // (divisor * 2^(shift+1)), with ties broken by picking the smallest
    // shift first. The winning byte is (shift << 4) | divisor_code.
    //
    // Pinning them here closes the hole where a numerator regression
    // from 524288 to 262144 (or a silent tie-break change) would still
    // pass the looser structural + one-octave-tolerance tests.
    //
    // C5 = MIDI 72 = 523.2511 Hz
    //   Target divisor-product = 524288 / 523.2511 ≈ 1002
    //   Closest achievable product = 1024 -> f = 512 Hz
    //   Smallest shift that hits 1024: shift=3, divisor code=4 (div=64)
    //   NR43 byte = (3 << 4) | 4 = 0x34
    CHECK_EQ(pitch_noise_nr43(0), std::uint8_t{0x34});

    // C4 = MIDI 60 = 261.6256 Hz
    //   Target product ≈ 2004, closest achievable = 2048 -> f = 256 Hz
    //   Smallest shift that hits 2048: shift=4, divisor code=4 (div=64)
    //   NR43 byte = (4 << 4) | 4 = 0x44
    CHECK_EQ(pitch_noise_nr43(-12), std::uint8_t{0x44});

    // C6 = MIDI 84 = 1046.5023 Hz
    //   Target product ≈ 501, closest achievable = 512 -> f = 1024 Hz
    //   Smallest shift that hits 512: shift=2, divisor code=4 (div=64)
    //   NR43 byte = (2 << 4) | 4 = 0x24
    CHECK_EQ(pitch_noise_nr43(12), std::uint8_t{0x24});
}

TEST_CASE("pitch helpers clamp to bass end when semitone_to_hz underflows to 0")
{
    // At extremely negative semitones, std::exp2 underflows to exactly
    // 0.0, which exercises the `freq_hz <= 0.0` guard inside
    // `hz_to_11bit` (and the analogous guard in pitch_noise_nr43).
    // Without those guards, the subsequent `numerator / 0` would
    // produce +inf and drive std::lround into its C11 "domain error,
    // unspecified return" case. Pinning the documented clamp here
    // keeps that path honest.
    const std::int16_t absurd = -20000;

    CHECK_EQ(pitch_square_11bit(absurd), std::uint16_t{0});
    CHECK_EQ(pitch_wave_11bit(absurd),   std::uint16_t{0});

    // The NR43 guard returns 0x00 (shift=0, divisor code=0, width bit
    // clear) rather than picking a "closest match" on a 0-Hz target.
    const std::uint8_t nr43 = pitch_noise_nr43(absurd);
    CHECK_EQ(nr43 & 0x08, 0);  // width bit still clear
}

TEST_CASE("pitch_noise_nr43 output frequency tracks the target note within one octave")
{
    // The brute-force search cannot always hit an exact musical pitch
    // (the NR43 frequency grid is coarse by design), but it should
    // always land within one octave of the target — otherwise the
    // algorithm is broken.
    const int kDivisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};

    for (int s = -36; s <= 36; ++s) {
        const std::uint8_t byte =
            pitch_noise_nr43(static_cast<std::int16_t>(s));
        const int shift    = (byte >> 4) & 0x0F;
        const int div_code = byte & 0x07;
        const double divisor = static_cast<double>(kDivisors[div_code]);
        const double f_lfsr  =
            524288.0 / (divisor * std::exp2(static_cast<double>(shift + 1)));

        const int    midi     = 72 + s;
        const double target   =
            440.0 * std::exp2(static_cast<double>(midi - 69) / 12.0);

        // Within one octave means ratio in [0.5, 2.0].
        const double ratio = f_lfsr / target;
        CAPTURE(s);
        CAPTURE(f_lfsr);
        CAPTURE(target);
        CHECK_GE(ratio, 0.5);
        CHECK_LE(ratio, 2.0);
    }
}
