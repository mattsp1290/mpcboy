// tests/core/taps_test.cpp
//
// Unit tests for rackboy_core/rackboy_taps — per-channel tap routing
// (DC blocker + NaN/denormal guard).
//
// Closes the "tap routing cross-channel isolation" item from
// bead rackboy-8vx (rackboy_core: doctest suite). Exercises the
// implementation introduced in bead rackboy-xpe.
//
// Verifies:
//   - taps_reset zeroes all state
//   - Single-channel processing removes DC offset over time
//   - NaN input flushed to zero at output
//   - Denormal input flushed to zero at output
//   - In-place processing (in == out) works
//   - Null input → zero output + state flush
//   - Cross-channel isolation (processing CH1 doesn't touch CH2..CH4 state)
//   - taps_process skips channels with null output buffers (state preserved)
//   - Filter state persists across multiple calls

#include "doctest/doctest.h"

#include "rackboy_taps.h"
#include "rackboy_voice.h"  // kNumVoices

#include <cmath>
#include <cstdint>
#include <cstring>

using rackboy::core::TapChannelState;
using rackboy::core::TapState;
using rackboy::core::taps_process_channel;
using rackboy::core::taps_process;
using rackboy::core::taps_reset;
using rackboy::core::kDcBlockerAlpha;
using rackboy::core::kDenormalThreshold;
using rackboy::core::kNumVoices;

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST_CASE("taps_reset: zeroes all channel state")
{
    TapState s;
    // Poison the state to make sure reset actually clears it.
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        s.ch[i].prev_in  = 1.5f;
        s.ch[i].prev_out = -0.7f;
    }

    taps_reset(s);

    for (std::size_t i = 0; i < kNumVoices; ++i) {
        INFO("channel = ", i);
        CHECK_EQ(s.ch[i].prev_in, 0.0f);
        CHECK_EQ(s.ch[i].prev_out, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Single-channel DC blocker behavior
// ---------------------------------------------------------------------------

TEST_CASE("taps_process_channel: DC offset decays toward zero over many samples")
{
    // A constant DC input (x[n] = 0.5) should decay through the
    // high-pass filter. The impulse response is:
    //   y[n] = x[n] - x[n-1] + alpha * y[n-1]
    // With x constant, after the first sample x - x_prev = 0, so
    // y[n] = alpha * y[n-1], which decays geometrically to 0.
    TapChannelState state = {0.0f, 0.0f};
    constexpr std::uint32_t N = 2000;
    float in[N];
    float out[N];
    for (std::uint32_t i = 0; i < N; ++i) in[i] = 0.5f;

    taps_process_channel(state, N, in, out);

    // After ~2000 samples at alpha=0.995, y should be very close to 0.
    // Early samples retain the initial edge response.
    CHECK_GT(std::fabs(out[0]), 0.4f);         // initial response visible
    CHECK_LT(std::fabs(out[N - 1]), 0.01f);    // decayed
}

TEST_CASE("taps_process_channel: NaN input replaced with zero")
{
    TapChannelState state = {0.0f, 0.0f};
    float in[4]  = { 0.1f, std::nanf(""), 0.2f, 0.1f };
    float out[4] = {0};

    taps_process_channel(state, 4, in, out);

    // All outputs must be finite (no NaN escaping the filter).
    for (std::uint32_t i = 0; i < 4; ++i) {
        INFO("i = ", i);
        CHECK_FALSE(std::isnan(out[i]));
    }
    // State must also be finite.
    CHECK_FALSE(std::isnan(state.prev_in));
    CHECK_FALSE(std::isnan(state.prev_out));
}

TEST_CASE("taps_process_channel: denormals flushed to zero")
{
    // Start with filter state already BELOW the denormal threshold but
    // non-zero, and zero input. Without the denormal guard, the filter
    // math (y = 0 - 0 + 0.995 * y_prev) would produce a sub-threshold
    // non-zero value; the `if (fabs(y) < kDenormalThreshold) y = 0.0f`
    // guard flushes it to exact 0.0f. Using 0.5 * kDenormalThreshold
    // ensures the first sample's computed y is below the threshold.
    TapChannelState state = {0.0f, 0.5f * kDenormalThreshold};
    float in[8]  = {0};
    float out[8];
    std::memset(out, 0x7F, sizeof(out));  // poison output with non-zero bits

    taps_process_channel(state, 8, in, out);

    // Output must be exact 0.0f, not just "small" — the guard flushes.
    for (std::uint32_t i = 0; i < 8; ++i) {
        INFO("i = ", i);
        CHECK_EQ(out[i], 0.0f);
    }
    // State prev_out must also have flushed to exactly zero.
    CHECK_EQ(state.prev_out, 0.0f);
}

TEST_CASE("taps_process_channel: in-place matches out-of-place bitwise")
{
    // A two-pass loop refactor would break aliasing safety silently
    // unless we compare aliased vs non-aliased bitwise.
    float src[8] = { 0.1f, 0.2f, 0.3f, 0.2f, 0.1f, 0.0f, -0.1f, -0.2f };

    TapChannelState s_oop = {0.0f, 0.0f};
    float in_oop[8], out_oop[8];
    std::memcpy(in_oop, src, sizeof(src));
    taps_process_channel(s_oop, 8, in_oop, out_oop);

    TapChannelState s_ip = {0.0f, 0.0f};
    float buf[8];
    std::memcpy(buf, src, sizeof(src));
    taps_process_channel(s_ip, 8, buf, buf);  // in == out

    for (std::uint32_t i = 0; i < 8; ++i) {
        INFO("i = ", i);
        CHECK_EQ(buf[i], out_oop[i]);
    }
    CHECK_EQ(s_ip.prev_in, s_oop.prev_in);
    CHECK_EQ(s_ip.prev_out, s_oop.prev_out);
}

TEST_CASE("taps_process_channel: NaN in filter state recovers (output guard)")
{
    // Corrupted filter state (uninitialized memory) must not produce a
    // NaN avalanche. The second isnan(y) guard inside the loop is the
    // only path that protects against this.
    TapChannelState state = {0.0f, std::nanf("")};
    float in[4]  = { 0.1f, 0.1f, 0.1f, 0.1f };
    float out[4] = {0};

    taps_process_channel(state, 4, in, out);

    for (std::uint32_t i = 0; i < 4; ++i) {
        INFO("i = ", i);
        CHECK_FALSE(std::isnan(out[i]));
    }
    CHECK_FALSE(std::isnan(state.prev_out));
}

TEST_CASE("taps_process_channel: n == 0 is a no-op")
{
    TapChannelState state = {0.5f, -0.25f};
    float out[1] = {0};
    float in[1]  = {1.0f};

    taps_process_channel(state, 0, in, out);

    CHECK_EQ(state.prev_in, 0.5f);
    CHECK_EQ(state.prev_out, -0.25f);
    CHECK_EQ(out[0], 0.0f);
}

TEST_CASE("taps_process_channel: null input produces zero output + flushed state")
{
    TapChannelState state = { 0.42f, -0.77f };  // non-zero poison
    float out[8];
    std::memset(out, 0xFF, sizeof(out));  // poison

    taps_process_channel(state, 8, nullptr, out);

    for (std::uint32_t i = 0; i < 8; ++i) {
        INFO("i = ", i);
        CHECK_EQ(out[i], 0.0f);
    }
    // Per docs: null input flushes state.
    CHECK_EQ(state.prev_in, 0.0f);
    CHECK_EQ(state.prev_out, 0.0f);
}

// ---------------------------------------------------------------------------
// Filter state persistence across calls
// ---------------------------------------------------------------------------

TEST_CASE("taps_process_channel: state persists across calls on same channel")
{
    // A step input applied twice with the same state should accumulate
    // filter history. Compare: one 8-sample call vs two 4-sample calls
    // with the same total input.
    TapChannelState state_combined = {0.0f, 0.0f};
    TapChannelState state_split    = {0.0f, 0.0f};
    float in[8]  = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    float out_combined[8];
    float out_split[8];

    taps_process_channel(state_combined, 8, in, out_combined);

    taps_process_channel(state_split,    4, in,     out_split);
    taps_process_channel(state_split,    4, in + 4, out_split + 4);

    // Both must produce the same output: the filter is stateful but
    // the call boundary should not affect the result.
    for (std::uint32_t i = 0; i < 8; ++i) {
        INFO("i = ", i);
        CHECK_EQ(out_combined[i], out_split[i]);
    }
}

// ---------------------------------------------------------------------------
// Cross-channel isolation (the key invariant from the bead)
// ---------------------------------------------------------------------------

TEST_CASE("taps_process: processing one channel does not touch others' state")
{
    TapState s;
    taps_reset(s);

    // Render into CH1 only; leave others untouched.
    float in_ch1[4]  = { 0.5f, 0.5f, 0.5f, 0.5f };
    float out_ch1[4] = {0};

    taps_process(s, 4,
                 in_ch1, nullptr, nullptr, nullptr,
                 out_ch1, nullptr, nullptr, nullptr);

    // CH1 state must have advanced.
    CHECK_NE(s.ch[0].prev_in, 0.0f);

    // CH2/CH3/CH4 state must still be zero — untouched.
    for (std::size_t i = 1; i < kNumVoices; ++i) {
        INFO("channel = ", i);
        CHECK_EQ(s.ch[i].prev_in, 0.0f);
        CHECK_EQ(s.ch[i].prev_out, 0.0f);
    }
}

TEST_CASE("taps_process: null output skips channel, preserving state")
{
    TapState s;
    taps_reset(s);
    // Poison CH2 state so we can see if it's touched.
    s.ch[1].prev_in  = 0.3f;
    s.ch[1].prev_out = 0.7f;

    float in_all[4]   = { 0.1f, 0.1f, 0.1f, 0.1f };
    float out_ch1[4]  = {0};

    // Only CH1 has an output buffer; others have null outputs.
    taps_process(s, 4,
                 in_all, in_all, in_all, in_all,
                 out_ch1, nullptr, nullptr, nullptr);

    // CH2 state MUST still be poisoned — taps_process skipped it.
    CHECK_EQ(s.ch[1].prev_in,  0.3f);
    CHECK_EQ(s.ch[1].prev_out, 0.7f);
    // CH3/CH4 also untouched (they started at zero from reset).
    CHECK_EQ(s.ch[2].prev_in,  0.0f);
    CHECK_EQ(s.ch[3].prev_in,  0.0f);
}

TEST_CASE("taps_process: each channel gets its own input stream")
{
    TapState s;
    taps_reset(s);

    // Distinct inputs per channel, identical lengths.
    float in1[2] = { 0.1f, 0.1f };
    float in2[2] = { 0.2f, 0.2f };
    float in3[2] = { 0.3f, 0.3f };
    float in4[2] = { 0.4f, 0.4f };
    float out1[2] = {0}, out2[2] = {0}, out3[2] = {0}, out4[2] = {0};

    taps_process(s, 2,
                 in1, in2, in3, in4,
                 out1, out2, out3, out4);

    // After 2 samples with distinct DC inputs, each channel's
    // prev_in must match its respective input (last element).
    CHECK_EQ(s.ch[0].prev_in, 0.1f);
    CHECK_EQ(s.ch[1].prev_in, 0.2f);
    CHECK_EQ(s.ch[2].prev_in, 0.3f);
    CHECK_EQ(s.ch[3].prev_in, 0.4f);
}

// ---------------------------------------------------------------------------
// Alpha coefficient sanity
// ---------------------------------------------------------------------------

TEST_CASE("kDcBlockerAlpha is a reasonable high-pass coefficient")
{
    CHECK_GT(kDcBlockerAlpha, 0.99f);
    CHECK_LT(kDcBlockerAlpha, 1.0f);
}
