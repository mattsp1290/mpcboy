// tests/core/clock_test.cpp
//
// Unit tests for rackboy_core/rackboy_clock.h — Gate 2 tick-all
// dispatch (bead rackboy-hvz).
//
// Verifies that tick_all():
//   * Calls table_tick() for each triggered voice with a valid table
//   * Skips untriggered voices
//   * Skips voices with kNoInstrument or out-of-bounds index
//   * Skips voices whose instrument has kNoTable or an unoccupied table
//   * Correctly looks up the table from instrument raw[6]
//   * Advances the table cursor on each tick

#include "doctest/doctest.h"

#include "rackboy_clock.h"
#include "rackboy_trigger.h"
#include "rackboy_voice.h"
#include "rackboy_table.h"
#include "rackboy_pitch.h"
#include "rackboy_apu.h"
#include "rackboy_sav_types.h"
#include "rackboy_constants.h"

#include <cstring>

using rackboy::core::kNumVoices;
using rackboy::core::kNoInstrument;
using rackboy::core::VoiceChannel;
using rackboy::core::VoiceState;
using rackboy::core::voices_reset_all;
using rackboy::core::trigger_all;
using rackboy::core::tick_all;
using rackboy::table::TableRegState;
using rackboy::table::table_reg_state_reset;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyTable;
using rackboy::sav::bank_reset;
using rackboy::sav::table_reset;
using rackboy::sav::kNoTable;
using rackboy::sav::kMaxTables;
using rackboy::sav::kTableRowCount;

static rackboy_apu_t make_apu()
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    return apu;
}

// Helper: set up a Pulse1 instrument with a table reference.
static void set_pulse1_with_table(rackboy::sav::RackboyInstrument& inst,
                                   uint8_t envelope,
                                   uint8_t table_slot)
{
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = rackboy::sav::kInstrumentTypePulse;
    inst.raw[1] = envelope;
    inst.raw[5] = 0x10;          // bit 4 = table enable
    inst.raw[6] = table_slot;
    inst.raw[7] = 0x80;          // 50% duty
}

// Helper: populate a simple table with distinct transpose values per row.
static void make_simple_table(RackboyTable& table)
{
    table_reset(table);
    table.occupied = true;
    for (std::size_t r = 0; r < kTableRowCount; ++r) {
        table.rows[r].transpose = static_cast<int8_t>(r);  // 0,1,2,...,15
    }
}

TEST_CASE("tick_all advances cursor for triggered voice with valid table")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Set up a Pulse1 instrument referencing table slot 0.
    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    // Populate table slot 0.
    make_simple_table(bank.tables[0]);
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    // First trigger via Gate 1 to set up the voice.
    trigger_all(voices, regs, bank, &apu);
    CHECK(voices[0].triggered);
    CHECK_EQ(voices[0].table_cursor, 0);

    // Gate 2 tick 1: should read row 0 (transpose 0) then advance cursor to 1.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 1);

    // Gate 2 tick 2: should read row 1 (transpose 1) then advance to 2.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);

    // pitch_offset should accumulate: row 0 added 0, row 1 added 1.
    // After trigger, pitch_offset was 0. Row 0 transpose is 0, row 1 is 1.
    CHECK_EQ(regs[0].pitch_offset, 0 + 0 + 1);
}

TEST_CASE("tick_all skips untriggered voices")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;
    make_simple_table(bank.tables[0]);
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;
    // Do NOT trigger — voice.triggered remains false from reset.

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    tick_all(voices, regs, bank, &apu);

    // Cursor should not have advanced.
    CHECK_EQ(voices[0].table_cursor, 0);
    CHECK_FALSE(voices[0].triggered);
    CHECK_EQ(regs[0].pitch_offset, 0);
}

TEST_CASE("tick_all skips voices with kNoInstrument")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);
    make_simple_table(bank.tables[0]);
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].triggered = true;  // pretend it was triggered
    // But active_instrument_index is kNoInstrument from reset.

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    tick_all(voices, regs, bank, &apu);

    CHECK_EQ(voices[0].table_cursor, 0);
}

TEST_CASE("tick_all skips voices whose instrument has kNoTable")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Instrument with kNoTable (0xFF) at raw[6].
    std::memset(&bank.pulse1_pool[0], 0, sizeof(bank.pulse1_pool[0]));
    bank.pulse1_pool[0].raw[0] = rackboy::sav::kInstrumentTypePulse;
    bank.pulse1_pool[0].raw[1] = 0xF0;
    bank.pulse1_pool[0].raw[6] = kNoTable;  // no table
    bank.pulse1_pool[0].raw[7] = 0x80;
    bank.pulse1_count = 1;
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    // Trigger first so voice is active.
    trigger_all(voices, regs, bank, &apu);
    CHECK(voices[0].triggered);
    CHECK_EQ(voices[0].table_cursor, 0);

    // Tick — should be no-op because instrument has no table.
    tick_all(voices, regs, bank, &apu);

    // Cursor stays at 0 (table_tick was not called).
    CHECK_EQ(voices[0].table_cursor, 0);
}

TEST_CASE("tick_all skips voices whose table slot is unoccupied")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Instrument references table slot 5, but that slot is not occupied.
    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 5);
    bank.pulse1_count = 1;
    // bank.tables[5].occupied is false from bank_reset.
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);
    tick_all(voices, regs, bank, &apu);

    // Cursor stays at 0.
    CHECK_EQ(voices[0].table_cursor, 0);
}

TEST_CASE("tick_all table cursor wraps at 16 rows")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;
    make_simple_table(bank.tables[0]);
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Tick 16 times to wrap the cursor.
    for (int t = 0; t < 16; ++t) {
        tick_all(voices, regs, bank, &apu);
    }

    // After 16 ticks, cursor should have wrapped back to 0.
    CHECK_EQ(voices[0].table_cursor, 0);
}

TEST_CASE("tick_all ticks multiple voices independently")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Pulse1 with table slot 0.
    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;
    make_simple_table(bank.tables[0]);

    // Noise with table slot 1.
    std::memset(&bank.noise_pool[0], 0, sizeof(bank.noise_pool[0]));
    bank.noise_pool[0].raw[0] = rackboy::sav::kInstrumentTypeNoise;
    bank.noise_pool[0].raw[1] = 0x80;      // envelope
    bank.noise_pool[0].raw[4] = 0x37;      // shape
    bank.noise_pool[0].raw[6] = 1;         // table slot 1
    bank.noise_count = 1;

    // Populate table slot 1 with different transpose values.
    table_reset(bank.tables[1]);
    bank.tables[1].occupied = true;
    for (std::size_t r = 0; r < kTableRowCount; ++r) {
        bank.tables[1].rows[r].transpose = static_cast<int8_t>(r * 2);
    }

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;  // Pulse1
    voices[0].target_pitch = 0;
    voices[3].active_instrument_index = 0;  // Noise
    voices[3].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    CHECK(voices[0].triggered);
    CHECK(voices[3].triggered);

    // Tick once.
    tick_all(voices, regs, bank, &apu);

    // Both should advance independently.
    CHECK_EQ(voices[0].table_cursor, 1);
    CHECK_EQ(voices[3].table_cursor, 1);

    // Pulse1 row 0 transpose = 0, Noise row 0 transpose = 0.
    // Pitch offsets should reflect the row 0 transpose + trigger pitch.
    CHECK_EQ(regs[0].pitch_offset, 0);  // trigger=0, row0 transpose=0
    CHECK_EQ(regs[3].pitch_offset, 0);  // trigger=0, row0 transpose=0

    // Tick again.
    tick_all(voices, regs, bank, &apu);

    CHECK_EQ(voices[0].table_cursor, 2);
    CHECK_EQ(voices[3].table_cursor, 2);

    // Pulse1 row 1 transpose=1, Noise row 1 transpose=2.
    CHECK_EQ(regs[0].pitch_offset, 0 + 1);
    CHECK_EQ(regs[3].pitch_offset, 0 + 2);
}

TEST_CASE("tick_all skips Pulse instrument with stale table slot but table-enable off")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Pulse instrument with table_enable bit OFF (raw[5] bit 4 = 0)
    // but a stale table slot value at raw[6] = 0 (non-kNoTable).
    std::memset(&bank.pulse1_pool[0], 0, sizeof(bank.pulse1_pool[0]));
    bank.pulse1_pool[0].raw[0] = rackboy::sav::kInstrumentTypePulse;
    bank.pulse1_pool[0].raw[1] = 0xF0;
    bank.pulse1_pool[0].raw[5] = 0x00;  // table enable bit OFF
    bank.pulse1_pool[0].raw[6] = 0;     // stale slot value pointing at table 0
    bank.pulse1_pool[0].raw[7] = 0x80;
    bank.pulse1_count = 1;

    // Populate table slot 0 so it would tick if reached.
    make_simple_table(bank.tables[0]);
    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    // Trigger to set up the voice.
    trigger_all(voices, regs, bank, &apu);
    CHECK(voices[0].triggered);

    // Tick — should NOT call table_tick because table enable is off.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 0);
}

TEST_CASE("delay command freezes cursor for N ticks then resumes")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    // Table with a D (delay) command on row 1, param=3 (freeze 3 ticks).
    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    // Row 0: transpose 0, no commands.
    bank.tables[0].rows[0].transpose = 0;
    // Row 1: transpose 1, cmd1 = 'D', param = 3.
    bank.tables[0].rows[1].transpose = 1;
    bank.tables[0].rows[1].cmd1 = 'D';
    bank.tables[0].rows[1].cmd1_param = 3;
    // Row 2: transpose 2, no commands.
    bank.tables[0].rows[2].transpose = 2;

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Tick 1: reads row 0 (transpose 0, no cmd), cursor advances to 1.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 1);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 0);

    // Tick 2: reads row 1 (transpose 1, cmd D param=3), cursor advances to 2,
    // then D command sets delay to 3.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 3);

    // Ticks 3-5: cursor frozen at 2, delay decrements each tick.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 2);

    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 1);

    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 0);

    // Tick 6: delay expired, reads row 2 (transpose 2), cursor advances to 3.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 3);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 0);
    // pitch_offset: row0=0, row1=1, row2=2 → total 3
    CHECK_EQ(regs[0].pitch_offset, 0 + 1 + 2);
}

TEST_CASE("delay command with param 0 is effectively no delay")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[0].transpose = 0;
    bank.tables[0].rows[0].cmd1 = 'D';
    bank.tables[0].rows[0].cmd1_param = 0;  // delay = 0
    bank.tables[0].rows[1].transpose = 5;

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Tick 1: row 0 with D param=0 — sets delay to 0 which is a no-op.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 1);
    CHECK_EQ(voices[0].table_delay_ticks_remaining, 0);

    // Tick 2: should advance normally (not frozen).
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 2);
}

TEST_CASE("hop command redirects cursor to target row")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    // Table: row 0 has no cmd, row 1 has H cmd pointing to row 5.
    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[0].transpose = 0;
    bank.tables[0].rows[1].transpose = 1;
    bank.tables[0].rows[1].cmd1 = 'H';
    bank.tables[0].rows[1].cmd1_param = 5;
    bank.tables[0].rows[5].transpose = 10;

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Tick 1: reads row 0 (no cmd), cursor advances to 1.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 1);

    // Tick 2: reads row 1 (transpose 1, H cmd → row 5).
    // Normal advance puts cursor at 2, then H overrides to 5.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 5);

    // Tick 3: reads row 5 (transpose 10), cursor advances to 6.
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 6);
    // pitch_offset: row0=0, row1=1, row5=10 → 11
    CHECK_EQ(regs[0].pitch_offset, 0 + 1 + 10);
}

TEST_CASE("hop command with out-of-range param is no-op (phrase-level form)")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[0].transpose = 0;
    bank.tables[0].rows[0].cmd1 = 'H';
    bank.tables[0].rows[0].cmd1_param = 19;  // >= 16, phrase-level → no-op

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);

    // Tick 1: reads row 0, H param=19 is out of range → no-op.
    // Cursor advances normally to 1 (the hop does not fire).
    tick_all(voices, regs, bank, &apu);
    CHECK_EQ(voices[0].table_cursor, 1);
}

TEST_CASE("hop to self does not deadlock (cursor stays at target row)")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    // Row 3 hops to itself — infinite loop, but audio thread must not hang.
    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[3].transpose = 0;
    bank.tables[0].rows[3].cmd1 = 'H';
    bank.tables[0].rows[3].cmd1_param = 3;

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;
    voices[0].table_cursor = 3;  // start at row 3
    voices[0].triggered = true;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    // Tick multiple times — should not hang, cursor stays at 3.
    for (int t = 0; t < 100; ++t) {
        tick_all(voices, regs, bank, &apu);
        CHECK_EQ(voices[0].table_cursor, 3);
    }
}

TEST_CASE("kill command silences voice and clears triggered flag")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    set_pulse1_with_table(bank.pulse1_pool[0], 0xF0, 0);
    bank.pulse1_count = 1;

    // Table row 1 has K command.
    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[0].transpose = 0;
    bank.tables[0].rows[1].transpose = 0;
    bank.tables[0].rows[1].cmd1 = 'K';
    bank.tables[0].rows[1].cmd1_param = 0;  // K00 = immediate kill

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[0].active_instrument_index = 0;
    voices[0].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);
    CHECK(voices[0].triggered);
    CHECK_EQ(regs[0].envelope, 0xF0);  // initial envelope from instrument

    // Tick 1: row 0, no commands. Voice still triggered.
    tick_all(voices, regs, bank, &apu);
    CHECK(voices[0].triggered);

    // Tick 2: row 1, K command fires → voice killed.
    tick_all(voices, regs, bank, &apu);
    CHECK_FALSE(voices[0].triggered);
    CHECK_EQ(regs[0].envelope, 0);  // envelope zeroed

    // Tick 3: should be no-op because voice is no longer triggered.
    tick_all(voices, regs, bank, &apu);
    CHECK_FALSE(voices[0].triggered);
}

TEST_CASE("kill command on Wave channel clears DAC power")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);

    // Wave instrument with table.
    std::memset(&bank.wave_pool[0], 0, sizeof(bank.wave_pool[0]));
    bank.wave_pool[0].raw[0] = rackboy::sav::kInstrumentTypeWave;
    bank.wave_pool[0].raw[1] = 0x20;   // volume bits
    bank.wave_pool[0].raw[5] = 0x10;   // table enable
    bank.wave_pool[0].raw[6] = 0;      // table slot 0
    bank.wave_count = 1;

    table_reset(bank.tables[0]);
    bank.tables[0].occupied = true;
    bank.tables[0].rows[0].transpose = 0;
    bank.tables[0].rows[0].cmd1 = 'K';
    bank.tables[0].rows[0].cmd1_param = 0;

    bank.loaded = true;

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    voices[2].active_instrument_index = 0;  // Wave is voice index 2
    voices[2].target_pitch = 0;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    trigger_all(voices, regs, bank, &apu);
    CHECK(voices[2].triggered);
    CHECK_EQ(regs[2].wave_volume, 0x20);

    // K command fires on row 0 tick.
    tick_all(voices, regs, bank, &apu);
    CHECK_FALSE(voices[2].triggered);
    CHECK_EQ(regs[2].wave_volume, 0);
}

TEST_CASE("tick_all with unloaded bank is a no-op")
{
    rackboy_apu_t apu = make_apu();
    RackboyBank bank;
    bank_reset(bank);
    // bank.loaded remains false.

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    // Force a triggered voice with an instrument — should still be skipped.
    voices[0].active_instrument_index = 0;
    voices[0].triggered = true;

    TableRegState regs[kNumVoices];
    for (auto& r : regs) table_reg_state_reset(r);

    tick_all(voices, regs, bank, &apu);

    CHECK_EQ(voices[0].table_cursor, 0);
    CHECK_EQ(regs[0].pitch_offset, 0);
}
