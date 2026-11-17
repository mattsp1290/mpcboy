// tests/table/chord_noop_test.cpp
//
// Tests for the LSDJ `C` (chord) table command. Per
// docs/DECISIONS.md D-001 chord is deferred to v1.1 and treated as a
// runtime no-op; the load-time per-instrument warning is covered by
// tests/table/chord_scan_test.cpp.
//
// The runtime no-op contract is: `C` must behave identically to the
// already-established T/G/M/O global no-ops on every channel. Rather
// than hard-coding which registers the pitch path touches per
// channel (which drifts if a future bead adds a pitch-path write),
// each test runs the same tick twice — once with `C`, once with
// `G` (known no-op) — and asserts byte-identical APU, VoiceState,
// and TableRegState results. If chord ever gains a side effect the
// parity assertion surfaces it immediately regardless of which
// register changes.

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
using rackboy::table::kChordCommandByte;

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

static rackboy::sav::RackboyInstrument make_wave_inst(uint8_t volume_bits)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeWave;
    inst.raw[1] = volume_bits;
    return inst;
}

static rackboy::sav::RackboyInstrument make_noise_inst(uint8_t envelope,
                                                        uint8_t shape)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypeNoise;
    inst.raw[1] = envelope;
    inst.raw[4] = shape;
    return inst;
}

static rackboy::sav::RackboyTable make_cmd_table(uint8_t cmd, uint8_t param,
                                                   int8_t transpose = 0)
{
    rackboy::sav::RackboyTable tbl;
    rackboy::sav::table_reset(tbl);
    tbl.occupied = true;
    tbl.rows[0].transpose   = transpose;
    tbl.rows[0].cmd1        = cmd;
    tbl.rows[0].cmd1_param  = param;
    return tbl;
}

// Run a tick with `cmd` (chord or a known no-op) and return snapshots of
// the post-tick APU io_registers, VoiceState, and TableRegState. The
// pre-tick setup is identical across invocations so the caller can
// byte-compare the results to prove two commands are runtime-equivalent.
struct TickSnapshot {
    uint8_t       io[256];
    VoiceState    voice;
    TableRegState regs;
};

static TickSnapshot tick_with_cmd(VoiceChannel ch,
                                    const rackboy::sav::RackboyInstrument& inst,
                                    uint8_t cmd,
                                    uint8_t param)
{
    rackboy_apu_t apu = make_apu();
    // Zero the whole struct (including padding) before the module's
    // reset helper touches the named fields. Both helpers write only
    // the named fields, so padding is left in whatever state the
    // stack happened to hold; memset to 0 makes the padding
    // byte-identical across independently constructed instances so
    // `memcmp` can be used as a total equality check below.
    VoiceState voice;
    std::memset(&voice, 0, sizeof(voice));
    voice_reset(voice);
    TableRegState regs;
    std::memset(&regs, 0, sizeof(regs));
    table_reg_state_reset(regs);

    table_trigger(voice, regs, ch, inst, /*semitones_from_base=*/0, &apu);

    auto tbl = make_cmd_table(cmd, param);
    table_tick(voice, regs, ch, tbl, &apu, /*bank=*/nullptr);

    TickSnapshot snap;
    std::memset(&snap, 0, sizeof(snap));
    std::memcpy(snap.io, apu.gb.io_registers, sizeof(snap.io));
    snap.voice = voice;
    snap.regs  = regs;
    return snap;
}

// The parity assertion: `C` must produce byte-identical APU, VoiceState,
// and TableRegState results to `G` (known global no-op) on the same
// channel with the same pre-state. Uses a non-zero param on both sides
// to surface any handler that writes `param` into a register.
static void check_chord_parity_with_G(VoiceChannel ch,
                                        const rackboy::sav::RackboyInstrument& inst,
                                        uint8_t param)
{
    const TickSnapshot chord = tick_with_cmd(ch, inst, kChordCommandByte, param);
    const TickSnapshot gnoop = tick_with_cmd(ch, inst, 'G',               param);

    CAPTURE(static_cast<int>(ch));
    CHECK_EQ(std::memcmp(chord.io,    gnoop.io,    sizeof(chord.io)),    0);
    CHECK_EQ(std::memcmp(&chord.voice, &gnoop.voice, sizeof(VoiceState)), 0);
    CHECK_EQ(std::memcmp(&chord.regs,  &gnoop.regs,  sizeof(TableRegState)), 0);
}

TEST_CASE("chord on CH1 (Pulse1) matches G no-op byte-for-byte")
{
    auto inst = make_pulse_inst(0xF0, 0x80);
    check_chord_parity_with_G(VoiceChannel::Pulse1, inst, 0xAB);
}

TEST_CASE("chord on CH2 (Pulse2) matches G no-op byte-for-byte")
{
    auto inst = make_pulse_inst(0xF0, 0x80);
    check_chord_parity_with_G(VoiceChannel::Pulse2, inst, 0xAB);
}

TEST_CASE("chord on CH3 (Wave) matches G no-op byte-for-byte")
{
    auto inst = make_wave_inst(0x40);
    check_chord_parity_with_G(VoiceChannel::Wave, inst, 0xCD);
}

TEST_CASE("chord on CH4 (Noise) matches G no-op byte-for-byte")
{
    auto inst = make_noise_inst(/*envelope=*/0xF0, /*shape=*/0x57);
    check_chord_parity_with_G(VoiceChannel::Noise, inst, 0xEF);
}

TEST_CASE("chord in slot 1 does not short-circuit a real command in slot 2")
{
    // Row carries C (slot 1) and D (slot 2, delay). A stray dispatcher
    // that `return`s instead of `break`s on chord would leave the
    // delay counter at zero and fail this check.
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
    tbl.rows[0].cmd1        = kChordCommandByte;   // deferred no-op
    tbl.rows[0].cmd1_param  = 0x10;
    tbl.rows[0].cmd2        = 'D';                 // delay 3 ticks
    tbl.rows[0].cmd2_param  = 3;

    table_tick(voice, regs, VoiceChannel::Pulse1, tbl, &apu, /*bank=*/nullptr);

    CHECK_EQ(voice.table_delay_ticks_remaining, static_cast<uint8_t>(3));
}

TEST_CASE("chord preserves row transpose (pitch path still applies)")
{
    // Chord must not swallow the row's transpose field. Parity check:
    // tick with chord+transpose=5 vs tick with G+transpose=5 — both
    // must apply +5 to pitch_offset identically.
    auto inst = make_pulse_inst(0xF0, 0x80);

    rackboy_apu_t apu_chord = make_apu();
    VoiceState   v_chord;   voice_reset(v_chord);
    TableRegState r_chord;  table_reg_state_reset(r_chord);
    table_trigger(v_chord, r_chord, VoiceChannel::Pulse1, inst, 0, &apu_chord);
    const int16_t pitch_pre = r_chord.pitch_offset;
    auto tbl_c = make_cmd_table(kChordCommandByte, 0xFF, /*transpose=*/5);
    table_tick(v_chord, r_chord, VoiceChannel::Pulse1, tbl_c, &apu_chord,
               /*bank=*/nullptr);

    rackboy_apu_t apu_g = make_apu();
    VoiceState   v_g;   voice_reset(v_g);
    TableRegState r_g;  table_reg_state_reset(r_g);
    table_trigger(v_g, r_g, VoiceChannel::Pulse1, inst, 0, &apu_g);
    auto tbl_g = make_cmd_table('G', 0xFF, /*transpose=*/5);
    table_tick(v_g, r_g, VoiceChannel::Pulse1, tbl_g, &apu_g, /*bank=*/nullptr);

    // Both paths applied transpose identically.
    CHECK_EQ(r_chord.pitch_offset, pitch_pre + 5);
    CHECK_EQ(r_chord.pitch_offset, r_g.pitch_offset);
    CHECK_EQ(r_chord.freq_11bit,   r_g.freq_11bit);
}
