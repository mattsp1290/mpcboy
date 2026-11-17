// tests/resampler/init_test.cpp
//
// Construction and coefficient-shape tests for the production
// resampler (bead rackboy-45x). Verifies that decimator_init
// produces a valid filter for every supported host rate and
// that the filter has the shape we expect from a symmetric
// Kaiser-windowed linear-phase low-pass.

#include "doctest/doctest.h"

#include "rackboy_resampler.hpp"

#include <cmath>
#include <vector>

using rackboy::resampler::Decimator;
using rackboy::resampler::decimator_init;
using rackboy::resampler::decimator_rate_supported;
using rackboy::resampler::kDecimationFactor;
using rackboy::resampler::kMaxFilterLength;

TEST_CASE("decimator_rate_supported accepts canonical rates") {
    CHECK(decimator_rate_supported(44100));
    CHECK(decimator_rate_supported(48000));
    CHECK(decimator_rate_supported(96000));
    CHECK(decimator_rate_supported(192000));

    CHECK_FALSE(decimator_rate_supported(0));
    CHECK_FALSE(decimator_rate_supported(22050));
    CHECK_FALSE(decimator_rate_supported(88200));
    CHECK_FALSE(decimator_rate_supported(384000));
}

TEST_CASE("decimator_init rejects unsupported rates") {
    Decimator d{};
    CHECK_FALSE(decimator_init(&d, 0));
    CHECK_FALSE(decimator_init(&d, 22050));
    CHECK_FALSE(decimator_init(&d, 32000));
    CHECK(d.filter_length == 0);
}

TEST_CASE("decimator_init produces a valid filter at every supported rate") {
    const unsigned rates[] = {44100, 48000, 96000, 192000};
    for (const unsigned host_rate : rates) {
        CAPTURE(host_rate);
        Decimator d{};
        REQUIRE(decimator_init(&d, host_rate));

        CHECK(d.host_rate == host_rate);
        CHECK(d.internal_rate ==
              host_rate * static_cast<unsigned>(kDecimationFactor));

        // Filter length is positive, odd, and within bounds.
        REQUIRE(d.filter_length > 0);
        REQUIRE(d.filter_length <= kMaxFilterLength);
        CHECK((d.filter_length % 2) == 1);

        // DC gain normalised to 1.0 within double precision.
        CHECK(d.dc_gain == doctest::Approx(1.0).epsilon(1e-9));

        // Sum of stored float coefficients is close to 1.0 too
        // (allowing for float rounding during the final cast).
        double sum = 0.0;
        for (int i = 0; i < d.filter_length; ++i) {
            sum += static_cast<double>(d.coeffs[i]);
        }
        CHECK(sum == doctest::Approx(1.0).epsilon(1e-6));
    }
}

TEST_CASE("coefficients are symmetric about the centre tap") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    const int N = d.filter_length;
    const int centre = (N - 1) / 2;
    // Symmetric FIR: coeffs[centre - k] == coeffs[centre + k].
    // Tolerance is one float ULP at the peak magnitude; the
    // centre tap is ~0.1 so ULP ~ 1e-8 relative to the tap
    // magnitude.
    for (int k = 1; k <= centre; ++k) {
        CAPTURE(k);
        CHECK(d.coeffs[centre - k] ==
              doctest::Approx(d.coeffs[centre + k]).epsilon(1e-6));
    }
}

TEST_CASE("centre tap is the largest coefficient") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    const int N = d.filter_length;
    const int centre = (N - 1) / 2;
    const float peak = d.coeffs[centre];
    REQUIRE(peak > 0.0f);
    for (int i = 0; i < N; ++i) {
        if (i == centre) continue;
        CHECK(d.coeffs[i] <= peak);
    }
}

TEST_CASE("coefficients outside the filter length are zeroed") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    for (int i = d.filter_length; i < kMaxFilterLength; ++i) {
        CHECK(d.coeffs[i] == 0.0f);
    }
}

TEST_CASE("higher host rate uses the same filter shape") {
    // For every supported rate we design the same normalised
    // filter (passband / stopband are defined relative to the
    // internal rate by construction). The filter length is
    // therefore identical across rates -- a sanity check that
    // the design code does not smuggle in rate-dependent
    // behaviour that would make the decimator's quality vary
    // with host rate. The actual rate dependence lives in the
    // internal rate passed to the APU upstream.
    Decimator a{};
    Decimator b{};
    REQUIRE(decimator_init(&a, 48000));
    REQUIRE(decimator_init(&b, 192000));
    CHECK(a.filter_length == b.filter_length);
    for (int i = 0; i < a.filter_length; ++i) {
        CHECK(a.coeffs[i] == b.coeffs[i]);
    }
}
