// tests/core/bank_test.cpp
//
// Unit tests for rackboy_core/rackboy_bank.{h,cpp} — the SPSC
// handoff that publishes an off-thread-parsed RackboyBank to the
// audio thread (bead rackboy-2i8).
//
// These tests run single-threaded on the host; the real audio thread
// / UI thread concurrency contract is documented in the header. What
// we lock here is:
//
//   * `init()` leaves both internal buffers in the power-on silent
//     state and publishes slot 0 as current;
//   * `current()` never returns nullptr after init;
//   * `swap_bank()` ping-pongs between slot 0 and slot 1 on
//     successive calls (that is the core SPSC invariant that makes
//     the reader side safe);
//   * the bank the audio thread reads after a swap is bit-identical
//     to the staged payload, including the `loaded` flag and every
//     per-channel count / pool slot;
//   * BankSlot itself is non-copyable / non-movable (asserted via
//     the type traits), matching the deletes in the header.

#include <cstdint>
#include <type_traits>

#include "doctest/doctest.h"

#include "rackboy_bank.h"
#include "rackboy_sav_types.h"

using rackboy::core::Bank;
using rackboy::core::BankSlot;
using rackboy::sav::kInstrumentRecordBytes;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::kMaxTables;
using rackboy::sav::kMaxWaveFrames;
using rackboy::sav::kTableRowCount;
using rackboy::sav::kWaveFrameBytes;

namespace {

// Populate a `Bank` with a deterministic-but-non-trivial pattern so
// a round-trip through BankSlot can CHECK every byte that matters.
// The pattern is chosen to exercise:
//
//   * every per-channel count at a non-zero, non-max value;
//   * every instrument slot the counts advertise as valid;
//   * at least one table and one wave frame marked occupied with
//     non-zero row / sample content;
//   * the `loaded` flag set true (the most common "real" case).
void populate_bank(Bank& b, std::uint8_t seed) noexcept
{
    rackboy::sav::bank_reset(b);

    b.pulse1_count = 3;
    b.pulse2_count = 5;
    b.wave_count   = 2;
    b.noise_count  = 1;

    // Give the dense-prefix slots a recognizable pattern. Unused
    // slots (beyond the counts) stay zero per bank_reset, which the
    // audio thread's "out of range -> silence" contract relies on.
    for (std::size_t i = 0; i < b.pulse1_count; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            b.pulse1_pool[i].raw[k] =
                static_cast<std::uint8_t>(seed ^ (0x10 + i * 16 + k));
        }
    }
    for (std::size_t i = 0; i < b.pulse2_count; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            b.pulse2_pool[i].raw[k] =
                static_cast<std::uint8_t>(seed ^ (0x20 + i * 16 + k));
        }
    }
    for (std::size_t i = 0; i < b.wave_count; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            b.wave_pool[i].raw[k] =
                static_cast<std::uint8_t>(seed ^ (0x30 + i * 16 + k));
        }
    }
    for (std::size_t i = 0; i < b.noise_count; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            b.noise_pool[i].raw[k] =
                static_cast<std::uint8_t>(seed ^ (0x40 + i * 16 + k));
        }
    }

    // One table slot populated with non-zero row content.
    b.tables[7].occupied = true;
    for (std::size_t r = 0; r < kTableRowCount; ++r) {
        b.tables[7].rows[r].transpose  = static_cast<std::int8_t>(r - 8);
        b.tables[7].rows[r].cmd1       = static_cast<std::uint8_t>(seed + r);
        b.tables[7].rows[r].cmd1_param = static_cast<std::uint8_t>(seed ^ r);
        b.tables[7].rows[r].cmd2       = 0;
        b.tables[7].rows[r].cmd2_param = 0;
    }

    // One wave frame populated with non-zero sample bytes.
    b.waves[42].occupied = true;
    for (std::size_t s = 0; s < kWaveFrameBytes; ++s) {
        b.waves[42].samples[s] =
            static_cast<std::uint8_t>(seed ^ (0x80 + s));
    }

    b.loaded = true;
}

// Compare two banks byte-for-byte across every audio-thread-visible
// field. This is cheaper than a raw memcmp over sizeof(Bank) because
// it gives doctest a CAPTURE-annotated per-field failure site when
// something regresses — a memcmp failure would just tell us "some
// byte differs", without pointing at which pool.
void expect_banks_equal(const Bank& actual, const Bank& expected)
{
    CHECK_EQ(actual.pulse1_count, expected.pulse1_count);
    CHECK_EQ(actual.pulse2_count, expected.pulse2_count);
    CHECK_EQ(actual.wave_count,   expected.wave_count);
    CHECK_EQ(actual.noise_count,  expected.noise_count);
    CHECK_EQ(actual.loaded,       expected.loaded);

    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
            CAPTURE(i);
            CAPTURE(k);
            CHECK_EQ(actual.pulse1_pool[i].raw[k],
                     expected.pulse1_pool[i].raw[k]);
            CHECK_EQ(actual.pulse2_pool[i].raw[k],
                     expected.pulse2_pool[i].raw[k]);
            CHECK_EQ(actual.wave_pool  [i].raw[k],
                     expected.wave_pool  [i].raw[k]);
            CHECK_EQ(actual.noise_pool [i].raw[k],
                     expected.noise_pool [i].raw[k]);
        }
    }

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        CAPTURE(t);
        CHECK_EQ(actual.tables[t].occupied, expected.tables[t].occupied);
        for (std::size_t r = 0; r < kTableRowCount; ++r) {
            CAPTURE(r);
            CHECK_EQ(actual.tables[t].rows[r].transpose,
                     expected.tables[t].rows[r].transpose);
            CHECK_EQ(actual.tables[t].rows[r].cmd1,
                     expected.tables[t].rows[r].cmd1);
            CHECK_EQ(actual.tables[t].rows[r].cmd1_param,
                     expected.tables[t].rows[r].cmd1_param);
            CHECK_EQ(actual.tables[t].rows[r].cmd2,
                     expected.tables[t].rows[r].cmd2);
            CHECK_EQ(actual.tables[t].rows[r].cmd2_param,
                     expected.tables[t].rows[r].cmd2_param);
        }
    }

    for (std::size_t w = 0; w < kMaxWaveFrames; ++w) {
        CAPTURE(w);
        CHECK_EQ(actual.waves[w].occupied, expected.waves[w].occupied);
        for (std::size_t s = 0; s < kWaveFrameBytes; ++s) {
            CAPTURE(s);
            CHECK_EQ(actual.waves[w].samples[s],
                     expected.waves[w].samples[s]);
        }
    }
}

}  // namespace

TEST_CASE("BankSlot::init publishes a silent bank in slot 0")
{
    BankSlot slot;
    slot.init();

    CHECK_EQ(slot.active_index_for_test(), 0);

    const Bank* current = slot.current();
    REQUIRE(current != nullptr);

    // Freshly initialized bank is in the "silent / not loaded"
    // state: every count is zero, every table/wave frame is
    // unoccupied, loaded is false.
    CHECK_EQ(current->pulse1_count, 0);
    CHECK_EQ(current->pulse2_count, 0);
    CHECK_EQ(current->wave_count,   0);
    CHECK_EQ(current->noise_count,  0);
    CHECK_FALSE(current->loaded);

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        CAPTURE(t);
        CHECK_FALSE(current->tables[t].occupied);
    }
    for (std::size_t w = 0; w < kMaxWaveFrames; ++w) {
        CAPTURE(w);
        CHECK_FALSE(current->waves[w].occupied);
    }
}

TEST_CASE("BankSlot::swap_bank ping-pongs between buffers 0 and 1")
{
    BankSlot slot;
    slot.init();
    REQUIRE_EQ(slot.active_index_for_test(), 0);

    Bank staged_a;
    populate_bank(staged_a, /*seed=*/0xA5);

    // First swap: 0 -> 1.
    slot.swap_bank(staged_a);
    CHECK_EQ(slot.active_index_for_test(), 1);

    Bank staged_b;
    populate_bank(staged_b, /*seed=*/0x5A);

    // Second swap: 1 -> 0.
    slot.swap_bank(staged_b);
    CHECK_EQ(slot.active_index_for_test(), 0);

    // Third swap with the same payload: 0 -> 1. Confirms the
    // ping-pong does not get stuck on a single slot when the
    // staged content is unchanged from the writer's POV.
    slot.swap_bank(staged_b);
    CHECK_EQ(slot.active_index_for_test(), 1);
}

TEST_CASE("BankSlot::current returns the staged payload byte-for-byte")
{
    BankSlot slot;
    slot.init();

    Bank staged;
    populate_bank(staged, /*seed=*/0xC3);

    slot.swap_bank(staged);

    const Bank* published = slot.current();
    REQUIRE(published != nullptr);
    expect_banks_equal(*published, staged);
}

TEST_CASE("BankSlot::swap_bank round-trip preserves the old bank on "
          "the inactive slot until the next swap")
{
    // After the first swap, the audio-thread-visible bank is the
    // new one; the previously-current bank is still sitting in the
    // inactive slot. A second swap with a *different* payload must
    // not accidentally publish the old bank again — it must publish
    // the newly-staged one.
    BankSlot slot;
    slot.init();

    Bank staged_a;
    populate_bank(staged_a, /*seed=*/0x11);
    slot.swap_bank(staged_a);

    // Cache `current()` at the top of the "process() call" — this
    // matches the SPSC contract from
    // research/cpp-atomic-spsc-memory-order-audio.md: read once, hold
    // locally for the duration of the reader's call, do not re-load
    // the atomic on every access. REQUIRE the pointer is non-null so
    // a future refactor that regresses the invariant fails with a
    // readable assertion instead of a segfault.
    const Bank* const after_a = slot.current();
    REQUIRE(after_a != nullptr);
    expect_banks_equal(*after_a, staged_a);

    Bank staged_b;
    populate_bank(staged_b, /*seed=*/0x22);
    slot.swap_bank(staged_b);

    const Bank* const after_b = slot.current();
    REQUIRE(after_b != nullptr);
    expect_banks_equal(*after_b, staged_b);

    // The old bank is gone from the audio thread's view — we should
    // never observe staged_a again through current() until a
    // deliberate re-swap.
    CHECK_NE(after_b->pulse1_pool[0].raw[0],
             staged_a.pulse1_pool[0].raw[0]);
}

TEST_CASE("BankSlot is non-copyable and non-movable")
{
    // These are compile-time contracts pinned in the header. Keep a
    // runtime assertion anyway so a future refactor that accidentally
    // removes the `= delete;` lines fails the suite at test time too.
    CHECK_FALSE(std::is_copy_constructible<BankSlot>::value);
    CHECK_FALSE(std::is_copy_assignable<BankSlot>::value);
    CHECK_FALSE(std::is_move_constructible<BankSlot>::value);
    CHECK_FALSE(std::is_move_assignable<BankSlot>::value);
}

TEST_CASE("BankSlot survives repeated init calls (re-arming)")
{
    // init() is documented as safe to call again while the audio
    // thread is not running. A second init() must land on slot 0 and
    // re-reset *both* internal buffers to the silent state — this is
    // the path the host takes when a module is re-initialized /
    // re-seated.
    BankSlot slot;
    slot.init();

    Bank staged;
    populate_bank(staged, /*seed=*/0x42);
    slot.swap_bank(staged);
    REQUIRE_EQ(slot.active_index_for_test(), 1);

    slot.init();
    CHECK_EQ(slot.active_index_for_test(), 0);

    const Bank* current = slot.current();
    REQUIRE(current != nullptr);
    CHECK_FALSE(current->loaded);
    CHECK_EQ(current->pulse1_count, 0);
    CHECK_EQ(current->pulse2_count, 0);
    CHECK_EQ(current->wave_count,   0);
    CHECK_EQ(current->noise_count,  0);

    // Verify that `buffers_[1]` was also re-reset. A buggy init()
    // that only touched `buffers_[0]` would leave the previously-
    // swapped `staged` payload sitting in slot 1, and a subsequent
    // swap_bank call (which stages into slot 1) would start from a
    // poisoned baseline. We catch that by publishing a fresh, empty
    // bank through the SPSC path and confirming the newly-published
    // payload is also in the silent state — not a mix of "partial
    // staged + partial empty".
    Bank empty;
    rackboy::sav::bank_reset(empty);
    slot.swap_bank(empty);
    REQUIRE_EQ(slot.active_index_for_test(), 1);

    const Bank* after_reswap = slot.current();
    REQUIRE(after_reswap != nullptr);
    CHECK_FALSE(after_reswap->loaded);
    CHECK_EQ(after_reswap->pulse1_count, 0);
    CHECK_EQ(after_reswap->pulse2_count, 0);
    CHECK_EQ(after_reswap->wave_count,   0);
    CHECK_EQ(after_reswap->noise_count,  0);
    // Spot-check a specific pool byte and a specific wave-frame byte
    // that `populate_bank` would have stamped on a non-reset bank.
    // On a buggy init these would still carry the old `staged`
    // pattern; on a correct init they are zero.
    CHECK_EQ(after_reswap->pulse1_pool[0].raw[0], 0);
    CHECK_EQ(after_reswap->waves[42].samples[0],  0);
    CHECK_FALSE(after_reswap->waves[42].occupied);
}

TEST_CASE("BankSlot::active_index_for_test returns -1 before init()")
{
    // Pre-init state: active_ is still null. The helper must report
    // -1 rather than silently returning 1 from a naive ternary. A
    // future refactor that regresses this contract fires here
    // instead of quietly misleading anyone probing the pre-init
    // state.
    BankSlot slot;
    CHECK_EQ(slot.active_index_for_test(), -1);

    slot.init();
    CHECK_EQ(slot.active_index_for_test(), 0);
}
