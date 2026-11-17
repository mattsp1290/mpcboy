// tests/core/voice_test.cpp
//
// Unit tests for rackboy_core/rackboy_voice.h — the per-voice state struct
// for the four GB audio channels (bead rackboy-a8f).
//
// This is the first real rackboy_core test binary, so it also acts as a
// template for future tests/core/ files: doctest TEST_CASE, SUBCASE for
// related assertions, CHECK_EQ for typed diagnostics.

#include <type_traits>

#include "doctest/doctest.h"

#include "rackboy_voice.h"

using rackboy::core::kNumVoices;
using rackboy::core::kTableRowCount;
using rackboy::core::kNoInstrument;
using rackboy::core::VoiceChannel;
using rackboy::core::VoiceState;
using rackboy::core::voice_reset;
using rackboy::core::voices_reset_all;

TEST_CASE("VoiceState reset produces silent, un-triggered voice")
{
    // Poison the struct with non-default values first so a trivial
    // zero-init that happens to match the reset contract cannot silently
    // pass the test.
    VoiceState v{};
    v.active_instrument_index     = 42;
    v.table_cursor                = 9;
    v.table_delay_ticks_remaining = 3;
    v.target_pitch                = -17;
    v.triggered                   = true;
    v.retrigger_period_ticks      = 4;
    v.retrigger_counter_ticks     = 2;
    v.vibrato_speed               = 5;
    v.vibrato_depth               = 3;
    v.vibrato_phase               = 128;

    voice_reset(v);

    CHECK_EQ(v.active_instrument_index, kNoInstrument);
    CHECK_EQ(v.table_cursor, 0);
    CHECK_EQ(v.table_delay_ticks_remaining, 0);
    CHECK_EQ(v.target_pitch, 0);
    CHECK_FALSE(v.triggered);
    CHECK_EQ(v.retrigger_period_ticks, 0);
    CHECK_EQ(v.retrigger_counter_ticks, 0);
    CHECK_EQ(v.vibrato_speed, 0);
    CHECK_EQ(v.vibrato_depth, 0);
    CHECK_EQ(v.vibrato_phase, 0);
}

TEST_CASE("voices_reset_all resets every entry in a 4-element array")
{
    VoiceState voices[kNumVoices];
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        voices[i].active_instrument_index     = static_cast<std::int16_t>(i);
        voices[i].table_cursor                = static_cast<std::uint8_t>(i + 1);
        voices[i].table_delay_ticks_remaining = static_cast<std::uint8_t>(i + 2);
        voices[i].target_pitch                = static_cast<std::int16_t>(-1 - i);
        voices[i].triggered                   = true;
        voices[i].retrigger_period_ticks      = static_cast<std::uint8_t>(i + 3);
        voices[i].retrigger_counter_ticks     = static_cast<std::uint8_t>(i + 4);
        voices[i].vibrato_speed               = static_cast<std::uint8_t>(i + 5);
        voices[i].vibrato_depth               = static_cast<std::uint8_t>(i + 6);
        voices[i].vibrato_phase               = static_cast<std::uint8_t>(i * 32);
    }

    voices_reset_all(voices);

    for (std::size_t i = 0; i < kNumVoices; ++i) {
        CAPTURE(i);
        CHECK_EQ(voices[i].active_instrument_index, kNoInstrument);
        CHECK_EQ(voices[i].table_cursor, 0);
        CHECK_EQ(voices[i].table_delay_ticks_remaining, 0);
        CHECK_EQ(voices[i].target_pitch, 0);
        CHECK_FALSE(voices[i].triggered);
        CHECK_EQ(voices[i].retrigger_period_ticks, 0);
        CHECK_EQ(voices[i].retrigger_counter_ticks, 0);
        CHECK_EQ(voices[i].vibrato_speed, 0);
        CHECK_EQ(voices[i].vibrato_depth, 0);
        CHECK_EQ(voices[i].vibrato_phase, 0);
    }
}

TEST_CASE("VoiceChannel enum values match GB APU channel ordering")
{
    // SameBoy channel indices are 0..3 for Pulse1, Pulse2, Wave, Noise. The
    // enum in rackboy_voice.h must match so code can cross from
    // `static_cast<std::uint8_t>(VoiceChannel::Wave)` into the APU shim
    // without a translation table.
    CHECK_EQ(static_cast<std::uint8_t>(VoiceChannel::Pulse1), 0);
    CHECK_EQ(static_cast<std::uint8_t>(VoiceChannel::Pulse2), 1);
    CHECK_EQ(static_cast<std::uint8_t>(VoiceChannel::Wave),   2);
    CHECK_EQ(static_cast<std::uint8_t>(VoiceChannel::Noise),  3);
}

TEST_CASE("kNumVoices and kTableRowCount match the spec constants")
{
    // Sanity-lock: these constants are load-bearing for every other bead
    // in the core tree. If somebody bumps them, this test fires loudly
    // so the downstream work gets re-audited. Typed literals keep the
    // comparison clean if tests/ ever enables -Wsign-compare / -Wconversion.
    CHECK_EQ(kNumVoices,     std::size_t{4});
    CHECK_EQ(kTableRowCount, std::uint8_t{16});
    CHECK_EQ(kNoInstrument,  std::int16_t{-1});
}

TEST_CASE("brace-init zeroes every field but does NOT produce a valid "
          "reset voice (documents the zero-init vs voice_reset asymmetry)")
{
    // Pinning the pitfall from ~/.claude/research/cpp-pod-trivially-copyable-audio.md:
    // `VoiceState v{};` zero-initializes every field, which means
    // `active_instrument_index == 0` — NOT `kNoInstrument == -1`. A
    // zero-inited voice would silently select instrument #0 instead of
    // staying silent. Callers must go through voice_reset(); this test
    // locks that contract so a future refactor cannot quietly swap the
    // reset helper for `VoiceState v{};` without failing the suite.
    VoiceState v{};

    CHECK_EQ(v.active_instrument_index, 0);
    CHECK_NE(v.active_instrument_index, kNoInstrument);
    CHECK_EQ(v.table_cursor, 0);
    CHECK_EQ(v.table_delay_ticks_remaining, 0);
    CHECK_EQ(v.target_pitch, 0);
    CHECK_FALSE(v.triggered);
    CHECK_EQ(v.retrigger_period_ticks, 0);
    CHECK_EQ(v.retrigger_counter_ticks, 0);
    CHECK_EQ(v.vibrato_speed, 0);
    CHECK_EQ(v.vibrato_depth, 0);
    CHECK_EQ(v.vibrato_phase, 0);

    // After voice_reset, the contract is honored.
    voice_reset(v);
    CHECK_EQ(v.active_instrument_index, kNoInstrument);
}

TEST_CASE("VoiceState satisfies the trivially-copyable + standard-layout "
          "POD contract the header advertises")
{
    // These are the same assertions the header carries as static_asserts;
    // repeating them at runtime gives a test-level failure signal if a
    // future toolchain ever disables -static_assert diagnostics or if the
    // struct is refactored into something that happens to pass the
    // compile-time check but fails the runtime trait query (shouldn't
    // happen, but cheap insurance).
    CHECK(std::is_trivially_copyable<VoiceState>::value);
    CHECK(std::is_standard_layout<VoiceState>::value);
}
