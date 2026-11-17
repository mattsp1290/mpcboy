// tests/table/chord_scan_test.cpp
//
// Tests for `scan_chord_references` + `format_chord_reference` — the
// load-time warning path that D-001 requires for every instrument
// whose assigned table references the deferred `C` (chord) command.
// The runtime no-op half is covered by tests/table/chord_noop_test.cpp.
//
// Coverage (scan):
//   - empty bank → 0 refs
//   - chord on cmd1 → 1 ref
//   - chord on cmd2 → 1 ref
//   - chord on both slots of one row → 1 ref (per-instrument dedup)
//   - chord on multiple rows → 1 ref (per-instrument dedup)
//   - pulse raw[5] bit-4 table-enable gate honored
//   - kNoTable skipped (no warning)
//   - out-of-range table slot skipped
//   - unoccupied table slot skipped
//   - dense-prefix honored (slot >= pool_count not scanned)
//   - one chord-using instrument per channel → 4 refs in P1/P2/W/N order
//   - two pulse1 instruments sharing a table → 2 refs (per-instrument)
//   - out_cap == 0 → count-only; returns total, writes nothing
//   - out_cap < total → return total, write out_cap refs
//   - out_cap == kMaxChordReferencesPerBank with full bank → no truncation
//   - nullptr out with out_cap == 0 is legal (count-only mode)
//
// Coverage (format):
//   - P1-0, P2-15, W-7, N-31 produce exact D-001 string
//   - Em-dash is U+2014 (0xE2 0x80 0x94)
//   - Truncation returns "would-have-written" length (snprintf contract)
//   - buf_len == 0 returns 0, does not dereference buf

#include "doctest/doctest.h"

#include "rackboy_table.h"
#include "rackboy_sav_types.h"
#include "rackboy_voice.h"

#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>

using rackboy::core::VoiceChannel;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyInstrument;
using rackboy::sav::RackboyTable;
using rackboy::sav::RackboyTableRow;
using rackboy::sav::bank_reset;
using rackboy::sav::kMaxTables;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::kTableRowCount;
using rackboy::sav::kNoTable;
using rackboy::sav::kInstrumentTypePulse;
using rackboy::sav::kInstrumentTypeWave;
using rackboy::sav::kInstrumentTypeNoise;

using rackboy::table::ChordReference;
using rackboy::table::scan_chord_references;
using rackboy::table::format_chord_reference;
using rackboy::table::kChordCommandByte;
using rackboy::table::kMaxChordReferencesPerBank;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

// Reserve the large RackboyBank on the heap to keep per-TEST_CASE
// stack usage modest (RackboyBank is ~tens of KiB). doctest test-case
// teardown frees it automatically via unique_ptr.
static std::unique_ptr<RackboyBank> make_fresh_bank()
{
    auto bank = std::make_unique<RackboyBank>();
    bank_reset(*bank);
    return bank;
}

// Build a Pulse instrument referencing `table_slot` with table-enable
// bit set (raw[5] bit 4). Type-byte is Pulse (0x00) per LSDJ.
static RackboyInstrument make_pulse_inst(std::uint8_t table_slot,
                                         bool table_enable = true)
{
    RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = kInstrumentTypePulse;
    inst.raw[1] = 0xF0;  // envelope (arbitrary)
    inst.raw[5] = table_enable ? 0x10 : 0x00;  // bit 4 = table enable
    inst.raw[6] = table_slot;
    return inst;
}

static RackboyInstrument make_wave_inst(std::uint8_t table_slot)
{
    RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = kInstrumentTypeWave;
    inst.raw[6] = table_slot;
    // Wave/Noise instruments do not check the enable bit; a non-0xFF
    // slot is sufficient.
    return inst;
}

static RackboyInstrument make_noise_inst(std::uint8_t table_slot)
{
    RackboyInstrument inst;
    std::memset(&inst, 0, sizeof(inst));
    inst.raw[0] = kInstrumentTypeNoise;
    inst.raw[6] = table_slot;
    return inst;
}

// Populate `bank.tables[slot]` with a chord-using table. `cmd1_chord`
// / `cmd2_chord` select which command slot(s) carry the chord byte;
// non-chord slots are zero ("no command").
static void put_chord_table(RackboyBank& bank,
                            std::uint8_t slot,
                            bool cmd1_chord,
                            bool cmd2_chord,
                            std::size_t row = 0)
{
    REQUIRE(slot < kMaxTables);
    REQUIRE(row  < kTableRowCount);
    RackboyTable& t = bank.tables[slot];
    rackboy::sav::table_reset(t);
    t.occupied = true;
    if (cmd1_chord) t.rows[row].cmd1 = kChordCommandByte;
    if (cmd2_chord) t.rows[row].cmd2 = kChordCommandByte;
}

// Populate `bank.tables[slot]` with a table whose command slots carry
// only non-chord commands — verifies the scanner does not false-flag
// an instrument whose table happens to be occupied but chord-free.
static void put_noop_table(RackboyBank& bank, std::uint8_t slot)
{
    REQUIRE(slot < kMaxTables);
    RackboyTable& t = bank.tables[slot];
    rackboy::sav::table_reset(t);
    t.occupied = true;
    t.rows[0].cmd1 = 'T';   // global no-op tempo, not chord
    t.rows[1].cmd2 = 'G';   // global no-op groove
}

// ---------------------------------------------------------------------------
// scan_chord_references — skip / dedup / ordering
// ---------------------------------------------------------------------------

TEST_CASE("scan: fresh bank returns 0 references")
{
    auto bank = make_fresh_bank();
    std::array<ChordReference, 8> out{};
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    CHECK_EQ(n, 0);
}

TEST_CASE("scan: single pulse1 instrument, chord on cmd1, one reference")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/3);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 3, /*cmd1=*/true, /*cmd2=*/false);

    std::array<ChordReference, 8> out{};
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    REQUIRE_EQ(n, 1);
    CHECK_EQ(out[0].channel,         VoiceChannel::Pulse1);
    CHECK_EQ(out[0].instrument_slot, static_cast<std::uint8_t>(0));
    CHECK_EQ(out[0].table_slot,      static_cast<std::uint8_t>(3));
}

TEST_CASE("scan: chord on cmd2 only also produces one reference")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/4);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 4, /*cmd1=*/false, /*cmd2=*/true);

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 1);
}

TEST_CASE("scan: chord on both slots of one row dedups to one reference")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/5);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 5, /*cmd1=*/true, /*cmd2=*/true);

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 1);
}

TEST_CASE("scan: chord on multiple rows of one table dedups to one reference")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/6);
    bank->pulse1_count   = 1;
    RackboyTable& t = bank->tables[6];
    rackboy::sav::table_reset(t);
    t.occupied = true;
    t.rows[0].cmd1 = kChordCommandByte;
    t.rows[7].cmd2 = kChordCommandByte;
    t.rows[15].cmd1 = kChordCommandByte;

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 1);
}

TEST_CASE("scan: pulse with table-enable bit clear is skipped")
{
    auto bank = make_fresh_bank();
    // Slot 2 is a chord table, but the instrument's raw[5] bit 4 is 0,
    // so the runtime tick path would skip this instrument. Scanner
    // must match that behavior: no warning for an instrument whose
    // table will never fire.
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/2,
                                           /*table_enable=*/false);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 2, /*cmd1=*/true, /*cmd2=*/false);

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

TEST_CASE("scan: kNoTable is skipped")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/kNoTable);
    bank->pulse1_count   = 1;
    // No table populated — kNoTable short-circuits before we reach
    // the occupied check.

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

TEST_CASE("scan: unoccupied table slot is skipped")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/9);
    bank->pulse1_count   = 1;
    // tables[9].occupied remains false from bank_reset().

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

TEST_CASE("scan: non-chord table is not false-flagged")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/1);
    bank->pulse1_count   = 1;
    put_noop_table(*bank, 1);

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

TEST_CASE("scan: dense-prefix honored — instruments beyond *_count ignored")
{
    auto bank = make_fresh_bank();
    // Slot 3 carries a chord-table instrument but pulse1_count == 3
    // (valid indices 0..2 only). The scanner must not peek at slot 3.
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/kNoTable);
    bank->pulse1_pool[1] = make_pulse_inst(/*table_slot=*/kNoTable);
    bank->pulse1_pool[2] = make_pulse_inst(/*table_slot=*/kNoTable);
    bank->pulse1_pool[3] = make_pulse_inst(/*table_slot=*/0);
    put_chord_table(*bank, 0, /*cmd1=*/true, /*cmd2=*/false);
    bank->pulse1_count   = 3;

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

TEST_CASE("scan: one chord-using instrument per channel → 4 refs in P1/P2/W/N order")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_count   = 1;
    bank->pulse2_pool[0] = make_pulse_inst(/*table_slot=*/1);
    bank->pulse2_count   = 1;
    bank->wave_pool[0]   = make_wave_inst(/*table_slot=*/2);
    bank->wave_count     = 1;
    bank->noise_pool[0]  = make_noise_inst(/*table_slot=*/3);
    bank->noise_count    = 1;
    put_chord_table(*bank, 0, true, false);
    put_chord_table(*bank, 1, true, false);
    put_chord_table(*bank, 2, false, true);
    put_chord_table(*bank, 3, true, true);

    std::array<ChordReference, 8> out{};
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    REQUIRE_EQ(n, 4);
    CHECK_EQ(out[0].channel, VoiceChannel::Pulse1);
    CHECK_EQ(out[0].table_slot, 0);
    CHECK_EQ(out[1].channel, VoiceChannel::Pulse2);
    CHECK_EQ(out[1].table_slot, 1);
    CHECK_EQ(out[2].channel, VoiceChannel::Wave);
    CHECK_EQ(out[2].table_slot, 2);
    CHECK_EQ(out[3].channel, VoiceChannel::Noise);
    CHECK_EQ(out[3].table_slot, 3);
}

TEST_CASE("scan: two pulse1 instruments sharing one chord table → two refs")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/7);
    bank->pulse1_pool[1] = make_pulse_inst(/*table_slot=*/7);
    bank->pulse1_count   = 2;
    put_chord_table(*bank, 7, true, false);

    std::array<ChordReference, 8> out{};
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    REQUIRE_EQ(n, 2);
    CHECK_EQ(out[0].instrument_slot, static_cast<std::uint8_t>(0));
    CHECK_EQ(out[1].instrument_slot, static_cast<std::uint8_t>(1));
    CHECK_EQ(out[0].table_slot, static_cast<std::uint8_t>(7));
    CHECK_EQ(out[1].table_slot, static_cast<std::uint8_t>(7));
}

TEST_CASE("scan: out_cap < total returns full count, writes up to out_cap")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_pool[1] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_pool[2] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_count   = 3;
    put_chord_table(*bank, 0, true, false);

    std::array<ChordReference, 2> out{};
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    CHECK_EQ(n, 3);                                   // total always honest
    CHECK_EQ(out[0].instrument_slot, static_cast<std::uint8_t>(0));
    CHECK_EQ(out[1].instrument_slot, static_cast<std::uint8_t>(1));
}

TEST_CASE("scan: out_cap == 0 is count-only and does not touch out")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 0, true, false);

    // Sentinel byte pattern. If the scanner writes past out_cap this
    // pattern would be disturbed.
    std::array<ChordReference, 1> out{};
    std::memset(out.data(), 0x5A, sizeof(out));
    const std::size_t n = scan_chord_references(*bank, out.data(), 0);
    CHECK_EQ(n, 1);
    // Pattern survives: out[0] untouched.
    std::array<std::uint8_t, sizeof(ChordReference)> expected{};
    expected.fill(0x5A);
    CHECK_EQ(std::memcmp(&out[0], expected.data(), expected.size()), 0);
}

TEST_CASE("scan: nullptr out with out_cap == 0 is legal (count-only)")
{
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 0, true, false);

    CHECK_EQ(scan_chord_references(*bank, nullptr, 0), 1);
}

TEST_CASE("scan: nullptr out with nonzero out_cap degrades to count-only")
{
    // The header describes the nullptr case as "legal iff out_cap
    // == 0" but the implementation clamps out_cap to 0 internally
    // when out is null rather than asserting. This test pins the
    // clamp so the defensive branch does not rot: a future
    // maintainer reading the header is otherwise free to swap the
    // clamp for a crash, breaking any caller that relied on the
    // silent degradation. The return value must still be the
    // correct total and no memory should be written (the clamp
    // guarantees the implementation never dereferences the null
    // pointer).
    auto bank = make_fresh_bank();
    bank->pulse1_pool[0] = make_pulse_inst(/*table_slot=*/0);
    bank->pulse1_count   = 1;
    put_chord_table(*bank, 0, true, false);

    CHECK_EQ(scan_chord_references(*bank, nullptr, 8), 1);
    CHECK_EQ(scan_chord_references(*bank, nullptr, 256), 1);
}

TEST_CASE("scan: full bank at kMaxChordReferencesPerBank writes all refs")
{
    // Fill every instrument pool to its max, have each instrument
    // reference a chord-using table. Verifies that the buffer size
    // constant (kMaxChordReferencesPerBank) is a real upper bound
    // and no truncation happens when the caller pre-allocated that
    // many entries.
    auto bank = make_fresh_bank();

    // Fill pulse1 → table slot 0, pulse2 → slot 1, wave → slot 2,
    // noise → slot 3. All four are the same chord table layout.
    put_chord_table(*bank, 0, true, false);
    put_chord_table(*bank, 1, true, false);
    put_chord_table(*bank, 2, true, false);
    put_chord_table(*bank, 3, true, false);

    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        bank->pulse1_pool[i] = make_pulse_inst(0);
        bank->pulse2_pool[i] = make_pulse_inst(1);
        bank->wave_pool[i]   = make_wave_inst(2);
        bank->noise_pool[i]  = make_noise_inst(3);
    }
    bank->pulse1_count = static_cast<std::uint8_t>(kMaxInstrumentsPerChannel);
    bank->pulse2_count = static_cast<std::uint8_t>(kMaxInstrumentsPerChannel);
    bank->wave_count   = static_cast<std::uint8_t>(kMaxInstrumentsPerChannel);
    bank->noise_count  = static_cast<std::uint8_t>(kMaxInstrumentsPerChannel);

    std::vector<ChordReference> out(kMaxChordReferencesPerBank);
    const std::size_t n = scan_chord_references(*bank, out.data(), out.size());
    REQUIRE_EQ(n, kMaxChordReferencesPerBank);

    // Spot-check the boundaries in the promised P1/P2/W/N order.
    CHECK_EQ(out[0].channel, VoiceChannel::Pulse1);
    CHECK_EQ(out[kMaxInstrumentsPerChannel - 1].channel,
             VoiceChannel::Pulse1);
    CHECK_EQ(out[kMaxInstrumentsPerChannel].channel, VoiceChannel::Pulse2);
    CHECK_EQ(out[2 * kMaxInstrumentsPerChannel].channel, VoiceChannel::Wave);
    CHECK_EQ(out[3 * kMaxInstrumentsPerChannel].channel, VoiceChannel::Noise);
    CHECK_EQ(out.back().instrument_slot,
             static_cast<std::uint8_t>(kMaxInstrumentsPerChannel - 1));
}

TEST_CASE("scan: out-of-range table slot is skipped (defense-in-depth)")
{
    // Pulse instrument whose raw[6] is an impossible slot value
    // (above kMaxTables). The parser would reject this at load time,
    // but the scanner is defensive — it must not index past the
    // tables[] array.
    auto bank = make_fresh_bank();
    auto inst = make_pulse_inst(/*table_slot=*/kNoTable);
    // Stomp raw[6] with a sentinel smaller than kNoTable but >=
    // kMaxTables. kMaxTables == 32, kNoTable == 0xFF, so 0x40 sits
    // in the gap.
    inst.raw[6] = static_cast<std::uint8_t>(kMaxTables + 16);
    bank->pulse1_pool[0] = inst;
    bank->pulse1_count   = 1;

    std::array<ChordReference, 8> out{};
    CHECK_EQ(scan_chord_references(*bank, out.data(), out.size()), 0);
}

// ---------------------------------------------------------------------------
// format_chord_reference — exact D-001 string, truncation, zero buffer
// ---------------------------------------------------------------------------

// Canonical D-001 line for slot 0 on Pulse1. The em-dash is U+2014
// encoded as 0xE2 0x80 0x94 in UTF-8 — the literal bytes below must
// match byte-for-byte.
static const char* kExpectedP1_0 =
    "[rackboy_table] instrument P1-0 uses chord command \xE2\x80\x94"
    " deferred to v1.1, treated as no-op";

TEST_CASE("format: P1-0 produces exact D-001 string")
{
    ChordReference ref{VoiceChannel::Pulse1, 0, 0};
    char buf[160] = {0};
    const std::size_t n = format_chord_reference(ref, buf, sizeof(buf));
    CHECK_EQ(n, std::strlen(kExpectedP1_0));
    CHECK_EQ(std::string(buf), std::string(kExpectedP1_0));
}

TEST_CASE("format: P2-15 renders the correct channel abbreviation and slot")
{
    ChordReference ref{VoiceChannel::Pulse2, 15, 0};
    char buf[160] = {0};
    const std::size_t n = format_chord_reference(ref, buf, sizeof(buf));
    CHECK_GT(n, 0);
    const char* needle = "instrument P2-15 uses chord command";
    CHECK(std::strstr(buf, needle) != nullptr);
}

TEST_CASE("format: Wave channel uses the W abbreviation")
{
    ChordReference ref{VoiceChannel::Wave, 7, 0};
    char buf[160] = {0};
    format_chord_reference(ref, buf, sizeof(buf));
    CHECK(std::strstr(buf, "instrument W-7 uses chord command") != nullptr);
}

TEST_CASE("format: Noise channel uses the N abbreviation and slot 31")
{
    ChordReference ref{VoiceChannel::Noise, 31, 0};
    char buf[160] = {0};
    format_chord_reference(ref, buf, sizeof(buf));
    CHECK(std::strstr(buf, "instrument N-31 uses chord command") != nullptr);
}

TEST_CASE("format: em-dash is U+2014 (UTF-8 0xE2 0x80 0x94)")
{
    ChordReference ref{VoiceChannel::Pulse1, 0, 0};
    char buf[160] = {0};
    format_chord_reference(ref, buf, sizeof(buf));
    // Find the em-dash bytes in the output.
    const unsigned char em[] = {0xE2, 0x80, 0x94, 0x00};
    CHECK(std::strstr(buf, reinterpret_cast<const char*>(em)) != nullptr);
}

TEST_CASE("format: buffer too small truncates and NUL-terminates")
{
    ChordReference ref{VoiceChannel::Pulse1, 0, 0};
    char buf[20];
    std::memset(buf, 'X', sizeof(buf));
    const std::size_t n = format_chord_reference(ref, buf, sizeof(buf));
    // snprintf contract: return is the "would-have-written" length.
    CHECK_EQ(n, std::strlen(kExpectedP1_0));
    // Result is NUL-terminated within the buffer.
    CHECK_EQ(buf[sizeof(buf) - 1], '\0');
    // No sentinel Xs remain in the first byte — snprintf wrote
    // something.
    CHECK_NE(buf[0], 'X');
}

TEST_CASE("format: buf_len == 0 returns 0 and does not write")
{
    ChordReference ref{VoiceChannel::Pulse1, 0, 0};
    // Passing nullptr is legal in the buf_len == 0 case; the function
    // must not dereference. Use a dummy stack var anyway to also
    // confirm we don't touch it even with a valid address.
    char sentinel = '#';
    const std::size_t n =
        format_chord_reference(ref, &sentinel, 0);
    CHECK_EQ(n, 0);
    CHECK_EQ(sentinel, '#');
}

TEST_CASE("format: buf_len == 1 NUL-terminates and returns full length")
{
    // snprintf contract boundary: buf_len == 1 writes exactly one
    // byte — the NUL — and still returns the "would-have-written"
    // length. The formatter inherits this contract because it
    // delegates to std::snprintf; pinning the boundary keeps the
    // contract from drifting if the formatter is ever rewritten
    // without snprintf.
    ChordReference ref{VoiceChannel::Pulse1, 0, 0};
    char buf[1] = {'X'};
    const std::size_t n = format_chord_reference(ref, buf, 1);
    CHECK_EQ(buf[0], '\0');
    CHECK_EQ(n, std::strlen(kExpectedP1_0));
}
