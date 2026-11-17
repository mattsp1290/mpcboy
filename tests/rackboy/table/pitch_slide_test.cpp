// tests/table/pitch_slide_test.cpp
//
// Integration tests for the L (pitch slide / portamento) table command
// (bead rackboy-h2z).
//
// Per docs/LSDJ_REFERENCE.md, rackboy's `L xx` linearly interpolates
// from the previous note's frequency to the current row's target over
// `xx` Gate 2 ticks. The target is the row's post-transpose pitch; the
// start is the pre-transpose pitch. L is handled inline in
// table_tick's Step 4.25 (not in dispatch_command) because it needs
// access to `row.transpose` to reconstruct the start.
//
// Verifies:
//   - L xx populates slide_target_pitch and slide_ticks_remaining
//   - On the L row, pitch_offset is snapped back to pre-transpose start
//   - Linear interpolation over N ticks lands exactly on target
//   - L00 (explicit) cancels an in-flight slide
//   - Gate 1 (table_trigger) resets slide state
//   - L in cmd2 slot works the same as cmd1
//   - Negative transpose (downward slide) works
//   - CH4 Noise NR43 is re-packed with slide-interpolated pitch
//   - dispatch_command 'L' case is a no-op (set-up lives in Step 4.25)

#include "doctest/doctest.h"

#include "rackboy_table.h"
#include "rackboy_voice.h"
#include "rackboy_pitch.h"
#include "rackboy_apu.h"
#include "rackboy_sav_types.h"

#include <cstring>

using rackboy::core::VoiceChannel;
using rackboy::core::VoiceState;
using rackboy::core::voice_reset;
using rackboy::table::TableRegState;
using rackboy::table::table_reg_state_reset;
using rackboy::table::table_trigger;
using rackboy::table::table_tick;

static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

static rackboy::sav::RackboyInstrument make_pulse_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = 0xF0;  // envelope: vol=15
    inst.raw[7] = 0x80;  // duty 50%
    return inst;
}

static rackboy::sav::RackboyInstrument make_noise_inst()
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = 0xF0;  // envelope
    inst.raw[4] = 0x37;  // NR43 shape
    return inst;
}

static rackboy::sav::RackboyTable make_empty_table()
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    return tbl;
}

// Build a table with a single L command at row 0, with the given
// transpose and slide length.
static rackboy::sav::RackboyTable make_l_table(int8_t transpose,
                                                 uint8_t ticks)
{
    auto tbl = make_empty_table();
    tbl.rows[0].transpose   = transpose;
    tbl.rows[0].cmd1        = 'L';
    tbl.rows[0].cmd1_param  = ticks;
    return tbl;
}

// ---------------------------------------------------------------------------
// State population on L dispatch
// ---------------------------------------------------------------------------

TEST_CASE("L arms slide state: slide_target_pitch = post-transpose, ticks_remaining = param")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Base note is 0 (C5). Transpose +5 → target is +5 semitones.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    REQUIRE(voice.slide_ticks_remaining == 0);
    REQUIRE(regs.pitch_offset == 0);

    auto tbl = make_l_table(/*transpose=*/5, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(voice.slide_target_pitch, 5);
    CHECK_EQ(voice.slide_ticks_remaining, 4);
    // On the L row, pitch_offset is snapped back to pre-transpose.
    CHECK_EQ(regs.pitch_offset, 0);
}

TEST_CASE("L on the L row writes freq at pre-transpose (start) pitch")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    // Capture the freq_11bit that the initial trigger wrote (= base pitch).
    const uint16_t freq_at_base = regs.freq_11bit;

    auto tbl = make_l_table(/*transpose=*/7, /*ticks=*/8);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // Post-L-dispatch: the running freq_11bit should be for the base
    // pitch (0 semitones) — NOT for +7 semitones.
    CHECK_EQ(regs.freq_11bit, freq_at_base);
    // And hardware NR13/NR14 should match too.
    const uint16_t freq_from_regs = rackboy::core::pitch_square_11bit(0);
    CHECK_EQ(regs.freq_11bit, freq_from_regs);
}

// ---------------------------------------------------------------------------
// Linear interpolation convergence
// ---------------------------------------------------------------------------

TEST_CASE("L 04 with transpose +4 converges to target in exactly 4 ticks (+1 per tick)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/4, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);
    REQUIRE(voice.slide_ticks_remaining == 4);

    auto empty = make_empty_table();

    // Tick 1: (4 - 0) / 4 = 1 → pitch_offset = 1, ticks = 3
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 1);
    CHECK_EQ(voice.slide_ticks_remaining, 3);

    // Tick 2: (4 - 1) / 3 = 1 → pitch_offset = 2, ticks = 2
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 2);
    CHECK_EQ(voice.slide_ticks_remaining, 2);

    // Tick 3: (4 - 2) / 2 = 1 → pitch_offset = 3, ticks = 1
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);
    CHECK_EQ(voice.slide_ticks_remaining, 1);

    // Tick 4 (final): (4 - 3) / 1 = 1 → pitch_offset = 4, ticks = 0
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 4);
    CHECK_EQ(voice.slide_ticks_remaining, 0);

    // Target was +4 semitones → hardware freq_11bit now matches pitch_square_11bit(4).
    CHECK_EQ(regs.freq_11bit, rackboy::core::pitch_square_11bit(4));
}

TEST_CASE("L 03 with transpose +10 lands on target exactly despite integer division rounding")
{
    // (10 / 3) = 3.33 — integer division would drift, so the auto-
    // correcting (target - current) / ticks_remaining pattern must
    // land exactly on target on the final tick.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/10, /*ticks=*/3);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);

    auto empty = make_empty_table();
    // Tick 1: (10 - 0) / 3 = 3 → 3, ticks=2
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);

    // Tick 2: (10 - 3) / 2 = 3 → 6, ticks=1
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 6);

    // Tick 3 (final): (10 - 6) / 1 = 4 → 10, ticks=0
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 10);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

TEST_CASE("L 01 is a single-tick slide (start → target immediately on next tick)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/6, /*ticks=*/1);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);
    REQUIRE(voice.slide_ticks_remaining == 1);

    auto empty = make_empty_table();
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    // (6 - 0) / 1 = 6 → pitch_offset = 6 in one tick.
    CHECK_EQ(regs.pitch_offset, 6);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// Sign: downward slide
// ---------------------------------------------------------------------------

TEST_CASE("L with negative transpose slides downward and lands on target")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    // Start at +12 semitones. L with transpose -12 should slide down
    // to 0.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 12, &apu);
    REQUIRE(regs.pitch_offset == 12);

    auto l_row = make_l_table(/*transpose=*/-12, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    // Snap back to pre-transpose start = 12.
    REQUIRE(regs.pitch_offset == 12);
    REQUIRE(voice.slide_target_pitch == 0);

    auto empty = make_empty_table();
    // Tick 1: (0 - 12) / 4 = -3 → 9
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 9);
    // Tick 2: (0 - 9) / 3 = -3 → 6
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 6);
    // Tick 3: (0 - 6) / 2 = -3 → 3
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);
    // Tick 4 final: (0 - 3) / 1 = -3 → 0
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 0);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L 00 (explicit cancel)
// ---------------------------------------------------------------------------

TEST_CASE("L 00 cancels an in-flight slide")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Start a slide of length 8.
    auto l_row = make_l_table(/*transpose=*/8, /*ticks=*/8);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(voice.slide_ticks_remaining == 8);

    // Advance one tick to put the slide mid-flight.
    auto empty = make_empty_table();
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    REQUIRE(voice.slide_ticks_remaining == 7);

    // Now send an explicit L 00 on row 2.
    auto cancel = make_empty_table();
    cancel.rows[2].transpose  = 0;
    cancel.rows[2].cmd1       = 'L';
    cancel.rows[2].cmd1_param = 0;
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, cancel, &apu, /*bank=*/nullptr);

    CHECK_EQ(voice.slide_ticks_remaining, 0);
    CHECK_EQ(voice.slide_target_pitch, 0);
}

// ---------------------------------------------------------------------------
// Gate 1 retrigger resets slide state
// ---------------------------------------------------------------------------

TEST_CASE("Gate 1 (table_trigger) resets an in-flight slide")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/6, /*ticks=*/6);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(voice.slide_ticks_remaining == 6);

    // Mid-slide Gate 1.
    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    CHECK_EQ(voice.slide_target_pitch, 0);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L in cmd2 slot
// ---------------------------------------------------------------------------

TEST_CASE("L in cmd2 slot arms the slide the same as in cmd1")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto tbl = make_empty_table();
    tbl.rows[0].transpose   = 3;
    tbl.rows[0].cmd2        = 'L';
    tbl.rows[0].cmd2_param  = 3;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.slide_target_pitch, 3);
    CHECK_EQ(voice.slide_ticks_remaining, 3);
    CHECK_EQ(regs.pitch_offset, 0);
}

// ---------------------------------------------------------------------------
// CH4 Noise path
// ---------------------------------------------------------------------------

TEST_CASE("L on CH4 Noise writes NR43 re-packed with slide-interpolated pitch")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_noise_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Noise, inst, 0, &apu);
    const uint8_t nr43_shape = regs.nr43_shape;

    auto l_row = make_l_table(/*transpose=*/4, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Noise, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);

    auto empty = make_empty_table();
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Noise, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 1);

    // Read NR43 directly from the APU and verify it matches the
    // pitched byte merged with the width bit.
    const uint8_t pitched = rackboy::core::pitch_noise_nr43(1);
    const uint8_t width_bit = nr43_shape & 0x08u;
    const uint8_t expected = static_cast<uint8_t>((pitched & ~0x08u) | width_bit);
    CHECK_EQ(apu.gb.io_registers[0x22], expected);
}

// ---------------------------------------------------------------------------
// dispatch_command 'L' is a no-op (setup lives in Step 4.25)
// ---------------------------------------------------------------------------

TEST_CASE("dispatch_command 'L' case does not set slide state directly")
{
    // This is an observable-behavior assertion, not a direct call into
    // the static dispatch_command. The contract: if L is absent from a
    // row (even with a non-zero transpose), no slide state is written
    // and pitch_offset stays at post-transpose.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto tbl = make_empty_table();
    tbl.rows[0].transpose  = 5;
    tbl.rows[0].cmd1       = 'D';   // Delay, not L
    tbl.rows[0].cmd1_param = 0;     // D 00 — effectively no-op

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
    CHECK_EQ(regs.pitch_offset, 5);   // transpose applied normally
}

// ---------------------------------------------------------------------------
// Mid-slide non-L row transpose (Option B: target re-points)
// ---------------------------------------------------------------------------

TEST_CASE("L: mid-slide transpose shifts both pitch and target, preserving slide delta")
{
    // Start slide target = +4 over 4 ticks from 0. Row 2 has transpose
    // +10 (no L). Option B: slide_target_pitch gets +10 too, so the
    // remaining delta is preserved and the slide still takes 4 total
    // ticks to close, ending at +14 instead of +4.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/4, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);
    REQUIRE(voice.slide_target_pitch == 4);
    REQUIRE(voice.slide_ticks_remaining == 4);

    auto empty = make_empty_table();
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    // Row 1 transpose = 0 → no target shift. (4-0)/4 = 1 → pitch=1.
    CHECK_EQ(voice.slide_target_pitch, 4);
    CHECK_EQ(regs.pitch_offset, 1);

    // Row 2 transpose = +10, no L. Step 4 bumps pitch by +10 to 11,
    // Step 4.25 bumps target by +10 to 14, then steps: (14-11)/3 = 1.
    // New pitch = 11 + 1 = 12, ticks_remaining = 2.
    auto row2 = make_empty_table();
    row2.rows[2].transpose = 10;
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, row2, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.slide_target_pitch, 14);
    CHECK_EQ(regs.pitch_offset, 12);
    CHECK_EQ(voice.slide_ticks_remaining, 2);

    // Row 3 empty. (14-12)/2 = 1 → pitch=13, ticks_remaining=1.
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 13);

    // Row 4 final: (14-13)/1 = 1 → pitch=14, ticks_remaining=0.
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 14);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L + D interactions
// ---------------------------------------------------------------------------

TEST_CASE("L: D on a mid-slide row freezes the slide; resumes after delay")
{
    // Slide in flight. A row with a positive D delay freezes table_tick
    // at Step 1's early return — neither transpose nor slide interpolate
    // on delay ticks. After the delay drains, the slide resumes and
    // still lands on target after its original tick count.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    auto l_row = make_l_table(/*transpose=*/4, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, l_row, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 0);

    // Row 1 has D 02 — freeze two ticks.
    auto d_row = make_empty_table();
    d_row.rows[1].cmd1        = 'D';
    d_row.rows[1].cmd1_param  = 2;
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, d_row, &apu, /*bank=*/nullptr);
    // First tick on row 1: D dispatches, sets delay=2. Slide interp
    // for this tick does run (delay was 0 entering this tick), so
    // pitch advances by one step.
    const int16_t pitch_after_d_row = regs.pitch_offset;
    CHECK_EQ(pitch_after_d_row, 1);
    CHECK_EQ(voice.slide_ticks_remaining, 3);
    CHECK_EQ(voice.table_delay_ticks_remaining, 2);

    // Delay tick #1: Step 1 early-returns. No slide step, no cursor
    // advance. delay_remaining decrements.
    table_tick(voice, regs, VoiceChannel::Pulse1, d_row, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, pitch_after_d_row);
    CHECK_EQ(voice.slide_ticks_remaining, 3);
    CHECK_EQ(voice.table_delay_ticks_remaining, 1);

    // Delay tick #2: same.
    table_tick(voice, regs, VoiceChannel::Pulse1, d_row, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, pitch_after_d_row);
    CHECK_EQ(voice.slide_ticks_remaining, 3);
    CHECK_EQ(voice.table_delay_ticks_remaining, 0);

    // Tick 4: empty row, slide continues. (4-1)/3 = 1 → pitch=2.
    auto empty = make_empty_table();
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 2);

    // Tick 5: (4-2)/2 = 1 → pitch=3.
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);

    // Tick 6 final: (4-3)/1 = 1 → pitch=4, ticks_remaining=0.
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 4);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L + R interaction
// ---------------------------------------------------------------------------

TEST_CASE("L: R recurring retrigger does not interfere with an in-flight slide")
{
    // LSDJ reference: "A table row that both slides and retriggers
    // queues the slide starting from the new note." The current design
    // honors that by arming a fresh L in Step 4.25 and letting Step 5's
    // R counter run independently. Retrigger fires (NRx4 bit 7 writes)
    // must NOT reset slide state.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Row 0: L 04 in cmd1, R 02 in cmd2.
    auto tbl = make_empty_table();
    tbl.rows[0].transpose   = 4;
    tbl.rows[0].cmd1        = 'L';
    tbl.rows[0].cmd1_param  = 4;
    tbl.rows[0].cmd2        = 'R';
    tbl.rows[0].cmd2_param  = 2;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    // L arms slide (target=4, ticks=4, pitch snapped to 0).
    // R arms retrigger (period=2, counter=2).
    REQUIRE(voice.slide_target_pitch == 4);
    REQUIRE(voice.slide_ticks_remaining == 4);
    REQUIRE(voice.retrigger_period_ticks == 2);
    REQUIRE(voice.retrigger_counter_ticks == 2);
    REQUIRE(regs.pitch_offset == 0);

    auto empty = make_empty_table();
    // Tick 1: slide step (+1 → 1), ticks_remaining=3. R counter
    // decrements to 1 (no fire yet).
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 1);
    CHECK_EQ(voice.slide_ticks_remaining, 3);
    CHECK_EQ(voice.retrigger_counter_ticks, 1);

    // Tick 2: slide step (+1 → 2), ticks_remaining=2. R counter hits 0
    // and re-arms to period=2 (fires NR14 trigger — but slide state is
    // untouched by the fire).
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 2);
    CHECK_EQ(voice.slide_ticks_remaining, 2);
    CHECK_EQ(voice.retrigger_counter_ticks, 2);  // reset to period
    CHECK_EQ(voice.slide_target_pitch, 4);  // NOT touched by R fire

    // Tick 3 + Tick 4: slide completes, R fires again on tick 4.
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 3);
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 4);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L replaces L mid-slide
// ---------------------------------------------------------------------------

TEST_CASE("L: new L on a mid-slide row replaces slide state (target + ticks)")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Slide 1: target +10 over 4 ticks.
    auto l1 = make_l_table(/*transpose=*/10, /*ticks=*/4);
    table_tick(voice, regs, VoiceChannel::Pulse1, l1, &apu, /*bank=*/nullptr);
    REQUIRE(voice.slide_target_pitch == 10);
    REQUIRE(regs.pitch_offset == 0);

    // Tick once: (10-0)/4 = 2 → pitch=2, remaining=3.
    auto empty = make_empty_table();
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    REQUIRE(regs.pitch_offset == 2);
    REQUIRE(voice.slide_ticks_remaining == 3);

    // Row 2: transpose +3, L 05. Step 4 lifts pitch to 5 (= 2 + 3).
    // Step 4.25: fresh L — target = pitch (5), remaining = 5, pitch
    // snapped back to 5 - 3 = 2.
    auto l2 = make_empty_table();
    l2.rows[2].transpose   = 3;
    l2.rows[2].cmd1        = 'L';
    l2.rows[2].cmd1_param  = 5;
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, l2, &apu, /*bank=*/nullptr);
    CHECK_EQ(voice.slide_target_pitch, 5);
    CHECK_EQ(voice.slide_ticks_remaining, 5);
    CHECK_EQ(regs.pitch_offset, 2);

    // Five ticks to land on 5.
    for (int i = 0; i < 5; ++i) {
        voice.table_cursor = static_cast<uint8_t>((i + 3) & 0x0F);
        table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    }
    CHECK_EQ(regs.pitch_offset, 5);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
}

// ---------------------------------------------------------------------------
// L + V composition
// ---------------------------------------------------------------------------

TEST_CASE("L: vibrato runs on top of slide — pitch_offset advances, freq reflects both")
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst();
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Row 0: transpose +4, L 04, V 0xF8 (speed=15, depth=8). Chosen so
    // the LFO puts vibrato_phase * depth / 128 well clear of zero
    // after just one tick — at small (phase, depth) combinations the
    // integer division rounds to 0 and the composition-with-slide
    // check becomes trivially true. speed=15/depth=8 puts offset ≈ 1
    // semitone after tick 1 and climbs from there.
    auto tbl = make_empty_table();
    tbl.rows[0].transpose   = 4;
    tbl.rows[0].cmd1        = 'L';
    tbl.rows[0].cmd1_param  = 4;
    tbl.rows[0].cmd2        = 'V';
    tbl.rows[0].cmd2_param  = 0xF8;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);
    // L armed, V armed after dispatch (V uses post-Step-4.5 advance
    // on *next* tick per the vibrato_test contract).
    REQUIRE(voice.slide_ticks_remaining == 4);
    REQUIRE(regs.pitch_offset == 0);
    REQUIRE(voice.vibrato_speed == 15);
    REQUIRE(voice.vibrato_depth == 8);

    auto empty = make_empty_table();
    // Tick 1: slide +1 → pitch_offset = 1. Vibrato phase advances by
    // 15 to 15. Vibrato offset at (phase=15, depth=8) rounds to a
    // non-zero positive semitone shift, so freq_11bit should differ
    // from pitch_square_11bit(pitch_offset) alone.
    voice.table_cursor = 1;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 1);
    CHECK_EQ(voice.vibrato_phase, 15);
    CHECK_NE(regs.freq_11bit, rackboy::core::pitch_square_11bit(1));

    // Tick 4 final: slide lands on +4.
    voice.table_cursor = 2;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    voice.table_cursor = 3;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    voice.table_cursor = 4;
    table_tick(voice, regs, VoiceChannel::Pulse1, empty, &apu, /*bank=*/nullptr);
    CHECK_EQ(regs.pitch_offset, 4);
    CHECK_EQ(voice.slide_ticks_remaining, 0);
    // vibrato_phase has advanced 4 times at speed 15 = 60 total.
    CHECK_EQ(voice.vibrato_phase, 60);
}
