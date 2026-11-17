// tests/core/gate_test.cpp
//
// Unit tests for rackboy_core/rackboy_gate.h — the Schmitt-trigger
// rising-edge detector for Gate 1 / Gate 2 inputs (bead rackboy-gn4).
//
// The core invariants under test:
//   * `gate_reset` produces a low state whose next high sample
//     fires an edge.
//   * Each low->high Schmitt transition emits exactly one `true` from
//     `gate_step`; no subsequent `true` until another low->high
//     transition.
//   * Noise bounded strictly inside the Schmitt band does not emit
//     edges (no zipper / chatter).
//   * A synthesized 1 kHz square-wave input at 48 kHz sample rate
//     produces exactly 1000 rising edges over one second of samples.
//   * Degenerate thresholds (rising <= falling) do not crash; the
//     detector collapses to a plain comparator.

#include <cmath>
#include <limits>

#include "doctest/doctest.h"

#include "rackboy_gate.h"

using rackboy::core::gate_reset;
using rackboy::core::gate_step;
using rackboy::core::GateState;
using rackboy::core::kDefaultGateFallingThreshold;
using rackboy::core::kDefaultGateRisingThreshold;

TEST_CASE("gate_reset puts the detector in a known low state")
{
    // Poison the state first so a trivial zero-init cannot silently
    // pass the test.
    GateState s{};
    s.high = true;

    gate_reset(s);

    CHECK_FALSE(s.high);

    // A subsequent step at a clearly-high input should fire an edge.
    CHECK(gate_step(s, 1.0f));
}

TEST_CASE("gate_step emits exactly one rising edge per low->high transition")
{
    GateState s{};
    gate_reset(s);

    // Walk through one full cycle: low, rising edge, sustained high,
    // falling edge, sustained low, rising edge again.
    CHECK_FALSE(gate_step(s, 0.0f));  // below falling threshold
    CHECK_FALSE(gate_step(s, 0.1f));  // still low
    CHECK     (gate_step(s, 0.9f));   // rising edge
    CHECK_FALSE(gate_step(s, 1.0f));  // sustained high, no new edge
    CHECK_FALSE(gate_step(s, 0.9f));  // still high
    CHECK_FALSE(gate_step(s, 0.15f)); // dropping, below falling
    CHECK_FALSE(gate_step(s, 0.0f));  // fully low
    CHECK     (gate_step(s, 0.85f));  // rising edge again
}

TEST_CASE("gate_step rejects noise bounded inside the Schmitt band")
{
    // With default thresholds 0.8 / 0.2, any noise in (0.2, 0.8) must
    // not alter the `high` state. Start from low; run a long noise
    // stream; assert no edges.
    GateState s{};
    gate_reset(s);

    const float noise_samples[] = {
        0.21f, 0.79f, 0.3f, 0.7f, 0.5f, 0.5f, 0.35f, 0.65f,
        0.22f, 0.78f, 0.45f, 0.55f, 0.25f, 0.75f, 0.50f, 0.50f,
    };
    for (const float v : noise_samples) {
        CAPTURE(v);
        CHECK_FALSE(gate_step(s, v));
    }
    CHECK_FALSE(s.high);

    // Now latch high and re-run the noise stream; state should stick.
    (void)gate_step(s, 0.95f);  // fire once
    REQUIRE(s.high);

    for (const float v : noise_samples) {
        CAPTURE(v);
        CHECK_FALSE(gate_step(s, v));
    }
    CHECK(s.high);
}

TEST_CASE("gate_step fires exactly at the rising threshold, not below")
{
    GateState s{};
    gate_reset(s);

    // Slow ramp into the Schmitt band. No edge until we reach exactly
    // kDefaultGateRisingThreshold (0.8f).
    for (int i = 0; i < 80; ++i) {
        const float v = static_cast<float>(i) * 0.01f;  // 0.00 .. 0.79
        CAPTURE(v);
        CHECK_FALSE(gate_step(s, v));
    }

    // The step at exactly the rising threshold should fire.
    CHECK(gate_step(s, kDefaultGateRisingThreshold));
    CHECK(s.high);
}

TEST_CASE("1 kHz square wave at 48 kHz produces exactly 1000 rising edges")
{
    // Synthesize 1 second of a 1 kHz square wave at the default 48 kHz
    // rackboy sample rate. Each period is 48 samples; the rising edge
    // is at sample 0 of each period. We should see exactly 1000
    // edges across the 48000-sample window.
    constexpr int   kSampleRate     = 48000;
    constexpr int   kSquareHz       = 1000;
    constexpr int   kSamplesPerPer  = kSampleRate / kSquareHz;  // 48
    constexpr int   kHighDuration   = kSamplesPerPer / 2;       // 24 samples high

    GateState s{};
    gate_reset(s);

    int edges = 0;
    for (int n = 0; n < kSampleRate; ++n) {
        const int phase = n % kSamplesPerPer;
        const float v   = (phase < kHighDuration) ? 1.0f : 0.0f;
        if (gate_step(s, v)) {
            ++edges;
        }
    }
    CHECK_EQ(edges, kSquareHz);
}

TEST_CASE("gate_step with equal thresholds (plain comparator mode) works")
{
    // rising == falling == 0.5. The Schmitt hysteresis collapses to
    // a plain comparator at that single threshold. Crossing up fires
    // a rising edge; crossing down drops silently.
    GateState s{};
    gate_reset(s);

    const float r  = 0.5f;
    const float f  = 0.5f;

    CHECK_FALSE(gate_step(s, 0.49f, r, f));
    CHECK     (gate_step(s, 0.51f, r, f));
    CHECK_FALSE(gate_step(s, 0.51f, r, f));
    CHECK_FALSE(gate_step(s, 0.49f, r, f));
    CHECK     (gate_step(s, 0.51f, r, f));
}

TEST_CASE("gate_step handles inverted thresholds (rising < falling) without chatter")
{
    // Before iteration 24's review-fix, passing rising < falling with
    // a constant mid-band input produced a rising edge every other
    // sample — the detector oscillated at (sample_rate / 2). The
    // implementation now swaps the thresholds locally on the stack
    // so the Schmitt contract still holds.
    //
    // Pin that behavior: feed 100 constant samples at 0.5f with
    // inverted thresholds and assert that AT MOST ONE edge fires
    // across the whole stream.
    GateState s{};
    gate_reset(s);

    const float inv_rising  = 0.3f;  // nominally "falling"
    const float inv_falling = 0.7f;  // nominally "rising"

    int edges = 0;
    for (int i = 0; i < 100; ++i) {
        if (gate_step(s, 0.5f, inv_rising, inv_falling)) {
            ++edges;
        }
    }
    // After the local swap, rising=0.7, falling=0.3. Input 0.5 is
    // inside the Schmitt band -> starts low (no edge), stays low.
    CHECK_EQ(edges, 0);
    CHECK_FALSE(s.high);

    // Now feed a sample above the (swapped) rising threshold — should
    // fire exactly one edge, then constant input should not re-fire.
    gate_reset(s);
    CHECK     (gate_step(s, 0.9f, inv_rising, inv_falling));
    CHECK_FALSE(gate_step(s, 0.9f, inv_rising, inv_falling));
    CHECK_FALSE(gate_step(s, 0.5f, inv_rising, inv_falling));  // inside band
    CHECK     (s.high);  // still latched
    CHECK_FALSE(gate_step(s, 0.2f, inv_rising, inv_falling));  // below falling, drops
    CHECK_FALSE(s.high);
}

TEST_CASE("gate_step handles NaN and +/-inf inputs")
{
    // NaN -> treated as 0.0f. +inf saturates high. -inf saturates low.
    GateState s{};
    gate_reset(s);

    const float nan = std::nanf("");
    const float pos_inf =  std::numeric_limits<float>::infinity();
    const float neg_inf = -std::numeric_limits<float>::infinity();

    // NaN on a low state: never fires.
    for (int i = 0; i < 50; ++i) {
        CHECK_FALSE(gate_step(s, nan));
    }
    CHECK_FALSE(s.high);

    // +inf on a low state: fires exactly one edge, stays latched.
    CHECK(gate_step(s, pos_inf));
    CHECK_FALSE(gate_step(s, pos_inf));
    CHECK(s.high);

    // NaN on a latched-high state: treated as 0.0, drops to low.
    CHECK_FALSE(gate_step(s, nan));
    CHECK_FALSE(s.high);

    // -inf on a low state: no edge, stays low.
    CHECK_FALSE(gate_step(s, neg_inf));
    CHECK_FALSE(s.high);

    // Fire and then -inf drops.
    (void)gate_step(s, 1.0f);
    REQUIRE(s.high);
    CHECK_FALSE(gate_step(s, neg_inf));
    CHECK_FALSE(s.high);
}

TEST_CASE("4 kHz square wave at 48 kHz produces exactly 4000 rising edges")
{
    // Higher-rate headroom test: 4 kHz is 4x the spec ceiling.
    // Period is 12 samples (6 high / 6 low). Exactly 4000 edges over
    // one second of samples proves the detector does not lose edges
    // at the ceiling and beyond.
    constexpr int   kSampleRate     = 48000;
    constexpr int   kSquareHz       = 4000;
    constexpr int   kSamplesPerPer  = kSampleRate / kSquareHz;  // 12
    constexpr int   kHighDuration   = kSamplesPerPer / 2;       // 6

    GateState s{};
    gate_reset(s);

    int edges = 0;
    for (int n = 0; n < kSampleRate; ++n) {
        const int phase = n % kSamplesPerPer;
        const float v   = (phase < kHighDuration) ? 1.0f : 0.0f;
        if (gate_step(s, v)) {
            ++edges;
        }
    }
    CHECK_EQ(edges, kSquareHz);
}

TEST_CASE("gate_step drops at exactly the falling threshold (mirror of rising-edge boundary)")
{
    // Mirror of the rising-edge exact-threshold test: latch high,
    // then step at exactly kDefaultGateFallingThreshold and assert
    // the state drops. Guards against a `<=` -> `<` typo regression
    // on the falling branch.
    GateState s{};
    gate_reset(s);

    (void)gate_step(s, 1.0f);
    REQUIRE(s.high);

    // Step just above falling threshold: still high.
    CHECK_FALSE(gate_step(s, kDefaultGateFallingThreshold + 0.01f));
    CHECK(s.high);

    // Step at exactly the falling threshold: drops.
    CHECK_FALSE(gate_step(s, kDefaultGateFallingThreshold));
    CHECK_FALSE(s.high);
}

TEST_CASE("gate_reset mid-stream clears stale high state")
{
    GateState s{};
    gate_reset(s);

    REQUIRE(gate_step(s, 1.0f));  // latch high
    REQUIRE(s.high);

    gate_reset(s);
    CHECK_FALSE(s.high);

    // After reset, the same high value must re-fire an edge (not be
    // treated as "already latched").
    CHECK(gate_step(s, 1.0f));
}

TEST_CASE("custom thresholds work independently of the defaults")
{
    // Simulate a tighter band appropriate for the clean VCV Rack CV path:
    // rising 0.6, falling 0.4.
    GateState s{};
    gate_reset(s);

    CHECK_FALSE(gate_step(s, 0.55f, 0.6f, 0.4f));
    CHECK     (gate_step(s, 0.60f, 0.6f, 0.4f));  // exact rising -> edge
    CHECK_FALSE(gate_step(s, 0.50f, 0.6f, 0.4f));  // inside new band, no change
    CHECK_FALSE(gate_step(s, 0.40f, 0.6f, 0.4f));  // exact falling -> drop
    CHECK_FALSE(s.high);
    CHECK     (gate_step(s, 0.65f, 0.6f, 0.4f));  // next rising edge
}

TEST_CASE("GateState trivially copyable + standard layout (POD contract)")
{
    CHECK(std::is_trivially_copyable<GateState>::value);
    CHECK(std::is_standard_layout<GateState>::value);
}
