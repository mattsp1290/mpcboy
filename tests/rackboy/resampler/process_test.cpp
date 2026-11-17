// tests/resampler/process_test.cpp
//
// Time-domain tests for the decimator: impulse response, DC
// preservation, silence, input validation.

#include "doctest/doctest.h"

#include "rackboy_resampler.hpp"

#include <cmath>
#include <vector>

using rackboy::resampler::Decimator;
using rackboy::resampler::decimator_init;
using rackboy::resampler::decimator_process;
using rackboy::resampler::decimator_reset_state;
using rackboy::resampler::kDecimationFactor;

namespace {

// Feed `in` through the decimator one host-sample-group
// (kDecimationFactor samples) at a time. Returns the
// accumulated output for channel 0. Channels 1..3 are driven
// with zero input.
std::vector<float> push_ch1(Decimator& d, const std::vector<float>& in) {
    std::vector<float> zero_ch(kDecimationFactor, 0.0f);
    std::vector<float> in_group(kDecimationFactor, 0.0f);
    std::vector<float> out1(1), out2(1), out3(1), out4(1);
    std::vector<float> accum;
    const int groups = static_cast<int>(in.size()) / kDecimationFactor;
    accum.reserve(static_cast<std::size_t>(groups));
    for (int g = 0; g < groups; ++g) {
        for (int j = 0; j < kDecimationFactor; ++j) {
            in_group[static_cast<std::size_t>(j)] =
                in[static_cast<std::size_t>(g * kDecimationFactor + j)];
        }
        const int n = decimator_process(
            &d, in_group.data(), zero_ch.data(), zero_ch.data(),
            zero_ch.data(), kDecimationFactor,
            out1.data(), out2.data(), out3.data(), out4.data());
        REQUIRE(n == 1);
        accum.push_back(out1[0]);
    }
    return accum;
}

}  // namespace

TEST_CASE("process rejects non-multiple-of-4 counts") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    float ignore[4] = {0, 0, 0, 0};
    const int r = decimator_process(
        &d, ignore, ignore, ignore, ignore, 3,
        ignore, ignore, ignore, ignore);
    CHECK(r == -1);
}

TEST_CASE("process with zero input is a legal no-op") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    float dummy = 0.0f;
    const int r = decimator_process(&d, &dummy, &dummy, &dummy, &dummy, 0,
                                    &dummy, &dummy, &dummy, &dummy);
    CHECK(r == 0);
}

TEST_CASE("silence in produces silence out") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    // Feed enough samples to flush the filter delay and then some.
    const int groups = (d.filter_length / kDecimationFactor) + 8;
    std::vector<float> in(static_cast<std::size_t>(groups * kDecimationFactor), 0.0f);
    const auto out = push_ch1(d, in);
    for (float x : out) {
        CHECK(x == 0.0f);
    }
}

TEST_CASE("DC input converges to DC output after the filter delay") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    const int groups = (d.filter_length / kDecimationFactor) + 32;
    std::vector<float> in(static_cast<std::size_t>(groups * kDecimationFactor), 1.0f);
    const auto out = push_ch1(d, in);

    // Samples after the full filter span should converge to 1.0.
    // Full settle requires all N input samples to be non-zero;
    // group delay (N-1)/2 only gets us to the peak of the
    // impulse response, not the tail. Convert "N input samples"
    // to "output groups" and add slack for emission alignment.
    const int settle_out =
        (d.filter_length + kDecimationFactor - 1) / kDecimationFactor + 2;
    REQUIRE(static_cast<int>(out.size()) > settle_out);

    // After the delay, every subsequent output should be 1.0
    // within float rounding of sum(coeffs).
    for (std::size_t i = static_cast<std::size_t>(settle_out); i < out.size(); ++i) {
        CAPTURE(i);
        CHECK(out[i] == doctest::Approx(1.0).epsilon(1e-5));
    }
}

TEST_CASE("impulse response matches filter coefficients (downsampled)") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    const int N = d.filter_length;
    const int M = kDecimationFactor;

    // A single unit impulse at the very first input sample. For
    // a 4:1 decimator, the emitted output at group g corresponds
    // to the dot product of the filter (in natural order) with
    // the ring window (oldest first) after M*(g+1) inputs have
    // been pushed. dot_ring pairs coeffs[0] with the OLDEST slot
    // and coeffs[N-1] with the NEWEST slot. The single non-zero
    // input at absolute index 0 has travelled M*(g+1) - 1
    // positions into the past by the time group g is emitted, so
    // it sits M*(g+1) - 1 positions from the newest slot, i.e.
    // at coefficient index (N - 1) - (M*(g+1) - 1) = N - M*(g+1).
    //
    // Once N - M*(g+1) < 0 the impulse has fallen off the end of
    // the ring and the output drops back to exactly 0. This pins
    // symmetry, indexing, phase, and cross-call boundary
    // alignment in one assertion -- an off-by-one in next_write
    // or a swapped oldest/newest ordering both fail here.

    const int groups = (N / M) + 8;
    std::vector<float> in(
        static_cast<std::size_t>(groups * M), 0.0f);
    in[0] = 1.0f;
    const auto out = push_ch1(d, in);
    REQUIRE(static_cast<int>(out.size()) == groups);

    for (int g = 0; g < groups; ++g) {
        const int tap_idx = N - M * (g + 1);
        const float expected =
            (tap_idx >= 0) ? d.coeffs[tap_idx] : 0.0f;
        CAPTURE(g);
        CAPTURE(tap_idx);
        CHECK(out[static_cast<std::size_t>(g)]
              == doctest::Approx(expected).epsilon(1e-6));
    }
}

TEST_CASE("decimator_reset_state clears history without touching coeffs") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    std::vector<float> snapshot(
        d.coeffs, d.coeffs + d.filter_length);

    // Drive the filter with DC so the history buffers fill.
    std::vector<float> dc_in(kDecimationFactor * 16, 1.0f);
    (void)push_ch1(d, dc_in);

    decimator_reset_state(&d);

    // Coeffs unchanged.
    for (int i = 0; i < d.filter_length; ++i) {
        CHECK(d.coeffs[i] == snapshot[static_cast<std::size_t>(i)]);
    }

    // History cleared: silence in, silence out immediately.
    std::vector<float> zero_in(kDecimationFactor * 4, 0.0f);
    const auto out = push_ch1(d, zero_in);
    for (float x : out) {
        CHECK(x == 0.0f);
    }
}

TEST_CASE("channels are independent") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));

    const int groups = (d.filter_length / kDecimationFactor) + 16;
    const int total = groups * kDecimationFactor;
    std::vector<float> dc1(static_cast<std::size_t>(total), 1.0f);
    std::vector<float> dc2(static_cast<std::size_t>(total), 2.0f);
    std::vector<float> zeros(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> o1(static_cast<std::size_t>(groups), 0.0f);
    std::vector<float> o2(static_cast<std::size_t>(groups), 0.0f);
    std::vector<float> o3(static_cast<std::size_t>(groups), 0.0f);
    std::vector<float> o4(static_cast<std::size_t>(groups), 0.0f);

    const int n = decimator_process(
        &d, dc1.data(), dc2.data(), zeros.data(), zeros.data(), total,
        o1.data(), o2.data(), o3.data(), o4.data());
    REQUIRE(n == groups);

    // After filter settles the ratios hold.
    const int settle =
        (d.filter_length + kDecimationFactor - 1) / kDecimationFactor + 4;
    for (int i = settle; i < groups; ++i) {
        CAPTURE(i);
        CHECK(o1[i] == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(o2[i] == doctest::Approx(2.0).epsilon(1e-5));
        CHECK(o3[i] == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(o4[i] == doctest::Approx(0.0).epsilon(1e-6));
    }
}
