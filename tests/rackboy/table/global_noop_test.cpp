// tests/table/global_noop_test.cpp
//
// Tests for the global no-op table commands T / G / M / O
// (bead rackboy-ydc).
//
// Per docs/LSDJ_REFERENCE.md's "Global no-op commands (v1)" table,
// these four letters are recognized so a loaded `.sav` parses
// cleanly, but rackboy deliberately executes them as no-ops and
// emits no load-time warning (rackboy does not own the state these
// commands would otherwise modify — tempo comes from Gate 2, groove
// is replaced by Gate 2 phrase scheduling, master volume and output
// routing are host concerns).
//
// This suite pins:
//   - T / G / M / O do not write any APU register
//   - T / G / M / O do not mutate VoiceState or TableRegState
//   - T / G / M / O interact cleanly with the row's transpose
//     (the pitch-path NRx3 / NRx4 writes still happen; only the
//     command itself is a no-op)

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

static rackboy::sav::RackboyInstrument make_pulse_inst(uint8_t envelope,
                                                        uint8_t duty)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = envelope;
    inst.raw[7] = duty;
    return inst;
}

static rackboy::sav::RackboyTable make_table_with_global_cmd(uint8_t cmd,
                                                               uint8_t param)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose   = 0;
    tbl.rows[0].cmd1        = cmd;
    tbl.rows[0].cmd1_param  = param;
    return tbl;
}

// Confirms that running a global no-op command produces no APU writes
// beyond what the pitch-path already does for this channel. Used as a
// helper so the four commands T / G / M / O all pass through the same
// assertions without duplicating setup.
static void check_global_cmd_is_noop(uint8_t cmd)
{
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    // Snapshot the post-trigger NRxx and VoiceState so we can verify
    // nothing changes beyond the pitch-path's tick update.
    const uint8_t nr10_pre  = apu.gb.io_registers[GB_IO_NR10];
    const uint8_t nr11_pre  = apu.gb.io_registers[GB_IO_NR11];
    const uint8_t nr12_pre  = apu.gb.io_registers[GB_IO_NR12];
    const uint8_t nr13_pre  = apu.gb.io_registers[GB_IO_NR13];
    const uint8_t nr14_pre  = apu.gb.io_registers[GB_IO_NR14];
    const uint8_t nr32_pre  = apu.gb.io_registers[GB_IO_NR32];
    const uint8_t nr42_pre  = apu.gb.io_registers[GB_IO_NR42];
    const uint8_t nr43_pre  = apu.gb.io_registers[GB_IO_NR43];
    const uint8_t nr44_pre  = apu.gb.io_registers[GB_IO_NR44];
    const uint8_t nr51_pre  = apu.gb.io_registers[GB_IO_NR51];
    const uint8_t nr52_pre  = apu.gb.io_registers[GB_IO_NR52];
    const VoiceState voice_pre = voice;
    const TableRegState regs_pre = regs;

    // Use a deliberately non-zero param byte so a stray handler that
    // writes `param` into some register would fail the test — a zero
    // param would match the zero-init state and hide the bug.
    auto tbl = make_table_with_global_cmd(cmd, 0xAB);
    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // Pitch-path registers (NR13 / NR14 no-trigger) MAY have been
    // re-written by the tick's Step 4 — that's expected and not part
    // of the global-noop contract. We only check that:
    //   (1) No register unrelated to the pitch path for this channel
    //       changed from its post-trigger snapshot.
    //   (2) NR14's trigger bit is clear after the tick (the global
    //       no-op must not set bit 7).
    //   (3) VoiceState / TableRegState are byte-identical to their
    //       pre-tick values aside from table_cursor advancing by 1.
    CAPTURE(static_cast<char>(cmd));
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], nr10_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR11], nr11_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR12], nr12_pre);
    // With transpose == 0 on row 0, the pitch path re-computes the
    // same freq_11bit, so the NR13 low byte is byte-identical. A
    // stray handler that wrote `param` into NR13 would fail here.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR13], nr13_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);       // no trigger
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x07, nr14_pre & 0x07);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR32], nr32_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR42], nr42_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR43], nr43_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR44], nr44_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR51], nr51_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR52], nr52_pre);

    // VoiceState: table_cursor advances from 0 to 1 (row 0 was read
    // then the cursor was bumped in Step 3 of table_tick). Every
    // other field must be unchanged.
    CHECK_EQ(voice.table_cursor, static_cast<uint8_t>(voice_pre.table_cursor + 1));
    CHECK_EQ(voice.active_instrument_index,    voice_pre.active_instrument_index);
    CHECK_EQ(voice.table_delay_ticks_remaining, voice_pre.table_delay_ticks_remaining);
    CHECK_EQ(voice.target_pitch,               voice_pre.target_pitch);
    CHECK_EQ(voice.triggered,                  voice_pre.triggered);
    CHECK_EQ(voice.retrigger_period_ticks,     voice_pre.retrigger_period_ticks);
    CHECK_EQ(voice.retrigger_counter_ticks,    voice_pre.retrigger_counter_ticks);

    // TableRegState: the pitch-path in table_tick re-computes
    // freq_11bit from regs.pitch_offset. With transpose=0 on row 0
    // the recomputation returns the same value, so the cache is
    // unchanged. Verify every field.
    CHECK_EQ(regs.freq_11bit,   regs_pre.freq_11bit);
    CHECK_EQ(regs.envelope,     regs_pre.envelope);
    CHECK_EQ(regs.duty,         regs_pre.duty);
    CHECK_EQ(regs.sweep,        regs_pre.sweep);
    CHECK_EQ(regs.nr43_shape,   regs_pre.nr43_shape);
    CHECK_EQ(regs.wave_volume,  regs_pre.wave_volume);
    CHECK_EQ(regs.pitch_offset, regs_pre.pitch_offset);

    // Whole-struct memcmp sentinel: auto-covers any new VoiceState
    // or TableRegState field a future bead adds without having to
    // extend this helper. Safe because both structs are statically
    // asserted trivially-copyable. Build voice_expected as the
    // pre-image with only table_cursor bumped by 1 (Step 3 of
    // table_tick advances the cursor), then compare bit-for-bit.
    VoiceState voice_expected = voice_pre;
    voice_expected.table_cursor =
        static_cast<std::uint8_t>(voice_pre.table_cursor + 1);
    CHECK_EQ(std::memcmp(&voice, &voice_expected, sizeof(VoiceState)), 0);
    CHECK_EQ(std::memcmp(&regs,  &regs_pre,       sizeof(TableRegState)), 0);
}

// Minimal wave instrument builder for the CH3 regression-guard test
// below. Matches the shape used in envelope_retrigger_test.cpp and
// retrigger_test.cpp: only raw[0] (type) and raw[1] (NR32 output
// level bits) are load-bearing for the no-op contract.
static rackboy::sav::RackboyInstrument make_wave_inst(uint8_t volume_bits)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = volume_bits;
    return inst;
}

TEST_CASE("T (tempo) is a no-op")
{
    check_global_cmd_is_noop('T');
}

TEST_CASE("G (groove) is a no-op")
{
    check_global_cmd_is_noop('G');
}

TEST_CASE("M (master volume) is a no-op")
{
    check_global_cmd_is_noop('M');
}

TEST_CASE("O (output routing) is a no-op")
{
    check_global_cmd_is_noop('O');
}

TEST_CASE("Global no-op is channel-independent (T on CH3 Wave also no-op)")
{
    // Channel-independence regression guard. The global-no-op group
    // sits above dispatch_command's per-channel switches and does
    // not branch on `ch`. A future refactor that accidentally gated
    // one of these letters on channel (e.g., wrapping in `if (ch ==
    // Pulse1)`) would fail this test. We pick CH3 (Wave) because it
    // has the richest per-channel register set (NR30 DAC, NR32
    // output level, wave RAM) so any accidental channel-specific
    // path is most likely to show up here.
    rackboy_apu_t apu = make_apu();
    auto inst = make_wave_inst(0x40);  // output level 100%
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Wave, inst, 0, &apu);

    const uint8_t nr30_pre = apu.gb.io_registers[GB_IO_NR30];
    const uint8_t nr32_pre = apu.gb.io_registers[GB_IO_NR32];
    const uint8_t nr33_pre = apu.gb.io_registers[GB_IO_NR33];
    const uint8_t nr34_pre = apu.gb.io_registers[GB_IO_NR34];
    const TableRegState regs_pre = regs;

    auto tbl = make_table_with_global_cmd('T', 0xCD);
    table_tick(voice, regs, VoiceChannel::Wave, tbl, &apu, /*bank=*/nullptr);

    // DAC, output level, and freq high bits must be unchanged; NR33
    // may be re-written by the pitch path but to the same value.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR30], nr30_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR32], nr32_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR33], nr33_pre);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x80, 0x00);   // no trigger
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR34] & 0x07, nr34_pre & 0x07);
    // Reg cache unchanged.
    CHECK_EQ(regs.wave_volume,  regs_pre.wave_volume);
    CHECK_EQ(regs.freq_11bit,   regs_pre.freq_11bit);
    CHECK_EQ(regs.pitch_offset, regs_pre.pitch_offset);
}

TEST_CASE("Global no-ops do not interfere with row transpose (pitch path still runs)")
{
    // A row that carries BOTH a transpose and a global no-op should
    // still apply the transpose to pitch_offset and update NR13/NR14
    // no-trigger, same as a row with no command at all.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);
    const int16_t pitch_pre = regs.pitch_offset;

    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose   = 5;    // +5 semitones
    tbl.rows[0].cmd1        = 'T';  // global no-op
    tbl.rows[0].cmd1_param  = 0xFF;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    // Transpose was applied and pitch path re-computed freq_11bit.
    CHECK_EQ(regs.pitch_offset, pitch_pre + 5);
    // NR14 bit 7 must still be clear (global no-op must not trigger).
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x80, 0x00);
    // NR14 low 3 bits reflect the updated freq_11bit high bits.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR14] & 0x07,
             (regs.freq_11bit >> 8) & 0x07);
}

TEST_CASE("Global no-op alongside a real command in the second slot still dispatches the real command")
{
    // Row carries both T (slot 1) and D (slot 2, delay). The no-op
    // must not short-circuit the dispatch of the second command —
    // the delay counter should be populated as if T were absent.
    rackboy_apu_t apu = make_apu();
    auto inst = make_pulse_inst(0xF0, 0x80);
    VoiceState voice;
    voice_reset(voice);
    TableRegState regs;
    table_reg_state_reset(regs);

    table_trigger(voice, regs, VoiceChannel::Pulse1, inst, 0, &apu);

    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose   = 0;
    tbl.rows[0].cmd1        = 'G';   // global no-op
    tbl.rows[0].cmd1_param  = 0x10;
    tbl.rows[0].cmd2        = 'D';   // delay 4 ticks
    tbl.rows[0].cmd2_param  = 4;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(voice.table_delay_ticks_remaining, 4);
}
