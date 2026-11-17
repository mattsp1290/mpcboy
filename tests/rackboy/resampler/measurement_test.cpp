// tests/resampler/measurement_test.cpp
//
// Unit-tests for the spectral measurement helpers promoted from
// src/spike/apu_bandlimited/main.cpp per docs/DECISIONS.md D-012
// item 4 and bead rackboy-dmq. Exercises each helper in isolation
// against synthetic signals so a regression here doesn't need a
// full APU rebuild to diagnose.

#include "doctest/doctest.h"

#include "measurement.hpp"
#include "spike/vcv_resampler/fft.h"

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

using rackboy::resampler::measurement::apply_bh4_window;
using rackboy::resampler::measurement::bh4_magnitude_db_fs;
using rackboy::resampler::measurement::goertzel_amplitude_db;
using rackboy::resampler::measurement::kBH4_a0;
using rackboy::resampler::measurement::max_harmonic_in_band;
using rackboy::resampler::measurement::max_odd_harmonic_in_band;
using rackboy::resampler::measurement::spur_floor_exact;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Build N samples of a unit-amplitude cosine at f_hz / fs sample
// rate. Phase 0. Used as a reference stimulus with a known 0 dBFS
// fundamental and nothing else in-band (up to DTFT leakage).
std::vector<float> make_cosine(double f_hz, unsigned fs, std::size_t N) {
    std::vector<float> out(N);
    const double w = 2.0 * kPi * f_hz / static_cast<double>(fs);
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<float>(std::cos(w * static_cast<double>(i)));
    }
    return out;
}

// FFT the signal into a half-spectrum ready for magnitude_db_fs or
// bh4_magnitude_db_fs. Wraps the spike FFT helper; not exposed as
// a measurement helper because the production path can assemble
// this inline.
std::vector<std::complex<float>> fft_of(const std::vector<float>& x) {
    std::vector<std::complex<float>> c(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        c[i] = std::complex<float>(x[i], 0.0f);
    }
    rackboy::spike::vcv_resampler::fft_forward(c);
    return c;
}

}  // namespace

TEST_CASE("apply_bh4_window matches the Harris-1978 coefficients at the "
          "centre and endpoints") {
    // Centre tap: a0 + a1 + a2 + a3 (cosine-sum at k=0.5, where
    // cos(pi) = -1 flips a1 and a3, cos(2*pi) = +1 keeps a2).
    //   w_centre = a0 + a1 + a2 + a3 = 1.0 by the coefficient
    //   normalisation Harris chose for the 4-term family.
    //
    // N = 1025 is chosen so the centre tap lands on the exact
    // integer index (N - 1) / 2 = 512 -- the window's symmetry
    // point. Any odd N works; 1025 is just a comfortable power-
    // of-two-plus-one for visual inspection in a debugger.
    std::vector<float> data(1025, 1.0f);
    apply_bh4_window(data);
    const std::size_t centre = (data.size() - 1) / 2;
    CHECK(data[centre] == doctest::Approx(1.0f).epsilon(1e-5));

    // Endpoints: a0 - a1 + a2 - a3 (cosine-sum at k=0 or k=1).
    const double endpoint =
        kBH4_a0 -
        rackboy::resampler::measurement::kBH4_a1 +
        rackboy::resampler::measurement::kBH4_a2 -
        rackboy::resampler::measurement::kBH4_a3;
    CHECK(static_cast<double>(data[0]) ==
          doctest::Approx(endpoint).epsilon(1e-5));
    CHECK(static_cast<double>(data[data.size() - 1]) ==
          doctest::Approx(endpoint).epsilon(1e-5));

    // Symmetry: w[k] == w[N-1-k] by construction (only cosines, k
    // enters as 2k/(N-1) - 1 which is symmetric).
    for (std::size_t k = 0; k < data.size() / 2; ++k) {
        CAPTURE(k);
        CHECK(data[k] == doctest::Approx(data[data.size() - 1 - k])
                             .epsilon(1e-5));
    }
}

TEST_CASE("apply_bh4_window is a no-op on inputs shorter than 2 samples") {
    std::vector<float> empty;
    apply_bh4_window(empty);
    CHECK(empty.empty());

    std::vector<float> single{42.0f};
    apply_bh4_window(single);
    CHECK(single[0] == doctest::Approx(42.0f));
}

TEST_CASE("bh4_magnitude_db_fs reads a unit cosine at 0 dBFS") {
    // BH4 windowing + FFT + dBFS conversion should put a unit
    // cosine's fundamental bin at ~0 dBFS, with BH4's coherent-
    // gain correction baked into the scale factor.
    constexpr std::size_t N = 1024;
    constexpr unsigned fs = 48000;
    // Pick an integer-bin tone so there's no scalloping loss.
    // Bin 64 = 64 * fs / N = 3000 Hz at fs=48000 N=1024.
    const double f0 = 64.0 * static_cast<double>(fs) /
                      static_cast<double>(N);

    std::vector<float> x = make_cosine(f0, fs, N);
    apply_bh4_window(x);
    const auto spec = fft_of(x);
    const auto db = bh4_magnitude_db_fs(spec);
    REQUIRE(db.size() == N / 2 + 1);

    // Peak should land at bin 64 and read within a few tenths of
    // a dB from 0 (BH4's amplitude correction is exact for an
    // on-grid cosine).
    double peak_db = -240.0;
    std::size_t peak_bin = 0;
    for (std::size_t k = 1; k < db.size(); ++k) {
        if (db[k] > peak_db) {
            peak_db = db[k];
            peak_bin = k;
        }
    }
    CHECK(peak_bin == 64u);
    CHECK(peak_db == doctest::Approx(0.0).epsilon(0.2));
}

TEST_CASE("bh4_magnitude_db_fs sidelobes are below -80 dBFS far from "
          "the main lobe") {
    // Verify the BH4 window suppresses an off-grid cosine's
    // spectral leakage: far from the main lobe, sidelobes must
    // sit well below -80 dBFS (the spike promised -92 dB below
    // main-lobe peak, which is -92 dBFS for a 0 dBFS cosine;
    // loosen to -80 dBFS as a robust regression bar).
    constexpr std::size_t N = 4096;
    constexpr unsigned fs = 48000;
    const double f0 = 1000.5;  // off-grid: between bins 85 and 86.

    std::vector<float> x = make_cosine(f0, fs, N);
    apply_bh4_window(x);
    const auto spec = fft_of(x);
    const auto db = bh4_magnitude_db_fs(spec);

    // Check bins well outside the main-lobe neighbourhood. The
    // fundamental is at bin ~85.5; skip +/- 6 bins and assert the
    // rest are below -80 dBFS. Hann's best is ~-31 dBFS; BH4 is
    // a >= 50 dB improvement on that.
    const double bin_hz = static_cast<double>(fs) /
                          static_cast<double>(N);
    const double fund_bin = f0 / bin_hz;
    for (std::size_t k = 1; k < db.size(); ++k) {
        if (std::abs(static_cast<double>(k) - fund_bin) < 6.0) {
            continue;
        }
        CHECK(db[k] < -80.0);
    }
}

TEST_CASE("max_harmonic_in_band and max_odd_harmonic_in_band are "
          "consistent with Nyquist") {
    // A 1 kHz tone at 48 kHz has a Nyquist of 24 kHz; max
    // integer harmonic is 24, max odd harmonic is 23.
    CHECK(max_harmonic_in_band(1000.0, 48000u) == 24);
    CHECK(max_odd_harmonic_in_band(1000.0, 48000u) == 23);

    // At 44100: Nyquist 22050; max h = 22, max odd = 21.
    CHECK(max_harmonic_in_band(1000.0, 44100u) == 22);
    CHECK(max_odd_harmonic_in_band(1000.0, 44100u) == 21);

    // Higher rates scale linearly.
    CHECK(max_harmonic_in_band(1000.0, 96000u) == 48);
    CHECK(max_harmonic_in_band(1000.0, 192000u) == 96);

    // Degenerate inputs clamp to h = 1.
    CHECK(max_harmonic_in_band(0.0, 48000u) == 1);
    CHECK(max_harmonic_in_band(1000.0, 0u) == 1);
    CHECK(max_odd_harmonic_in_band(1.0, 2u) == 1);  // floor(1) = 1

    // Branch coverage: `max_odd_harmonic_in_band`'s even->odd
    // subtraction. 24000 / 4000 = 6 (even) -> step down to 5.
    CHECK(max_odd_harmonic_in_band(4000.0, 48000u) == 5);
    // Branch coverage: a fundamental so high that `floor(Nyquist /
    // f0)` is 0 or 1; the floor clamps max_h up to 1.
    CHECK(max_odd_harmonic_in_band(25000.0, 48000u) == 1);
    CHECK(max_odd_harmonic_in_band(48000.0, 48000u) == 1);
}

TEST_CASE("spur_floor_exact excludes exact harmonic bins of an off-grid "
          "fundamental") {
    // Construct a synthetic spectrum with known energy at the
    // fundamental, at harmonics h=3/5/7 (odd-only, square-wave
    // style), and at one extra non-harmonic bin. Verify the
    // helper finds the non-harmonic without being fooled by
    // off-grid round-off in the harmonic positions.
    constexpr std::size_t N = 32768;
    constexpr unsigned fs = 96000;
    const double f0 = 1000.549;  // off-grid, like the APU Pulse1 stimulus

    // Half-spectrum length (what bh4_magnitude_db_fs returns).
    std::vector<float> db(N / 2 + 1, -120.0f);
    const double bin_hz = static_cast<double>(fs) /
                          static_cast<double>(N);

    auto place_tone = [&](double f_hz, double db_val) {
        // Put all the energy at the single nearest bin. Exact-
        // frequency exclusion is what the helper must tolerate;
        // near-nearest approximation is fine for the synthetic
        // stimulus here.
        const std::size_t k = static_cast<std::size_t>(
            std::round(f_hz / bin_hz));
        if (k < db.size()) {
            db[k] = static_cast<float>(db_val);
        }
    };

    // Fundamental + three odd harmonics all at -15 dBFS.
    place_tone(f0,       -15.0);
    place_tone(3.0 * f0, -30.0);
    place_tone(5.0 * f0, -35.0);
    place_tone(7.0 * f0, -40.0);
    // Non-harmonic spur midway between harmonics 3 and 5 (e.g.
    // 4 * f0 is an even-harmonic neighbourhood that must NOT be
    // excluded when `odd_harmonics_only == true`).
    place_tone(4.0 * f0, -50.0);

    const double worst_with_odd_only =
        spur_floor_exact(db, f0, fs, N,
                         /*half_width_bins=*/5,
                         /*max_harmonic=*/max_odd_harmonic_in_band(f0, fs),
                         /*odd_harmonics_only=*/true);
    // With odd-only exclusion, 4*f0 is NOT a harmonic so it is
    // the worst spur.
    CHECK(worst_with_odd_only == doctest::Approx(-50.0).epsilon(0.5));

    const double worst_all_harmonics =
        spur_floor_exact(db, f0, fs, N,
                         /*half_width_bins=*/5,
                         /*max_harmonic=*/max_harmonic_in_band(f0, fs),
                         /*odd_harmonics_only=*/false);
    // With all-harmonic exclusion, 4*f0 is excluded. The worst
    // non-harmonic should then fall back to the -120 dBFS floor.
    CHECK(worst_all_harmonics == doctest::Approx(-120.0).epsilon(0.5));
}

TEST_CASE("spur_floor_exact is safe when max_harmonic exceeds Nyquist") {
    // The docstring states the precondition max_harmonic <=
    // floor(Nyquist / f0), but passing a value above that must
    // not crash or assert -- over-band harmonics land on bins
    // outside the spectrum and are effectively ignored. Verify
    // the helper returns the same answer as when the caller
    // clamped correctly to max_(odd_)harmonic_in_band.
    constexpr std::size_t N = 8192;
    constexpr unsigned fs = 48000;
    const double f0 = 1000.0;

    std::vector<float> db(N / 2 + 1, -120.0f);
    const double bin_hz = static_cast<double>(fs) /
                          static_cast<double>(N);
    const std::size_t k_fund = static_cast<std::size_t>(
        std::round(f0 / bin_hz));
    db[k_fund] = -10.0f;
    // Plant a legitimate non-harmonic spur at 5500 Hz (>= f0, not
    // an integer multiple).
    const std::size_t k_spur = static_cast<std::size_t>(
        std::round(5500.0 / bin_hz));
    db[k_spur] = -70.0f;

    const int correct_max = max_odd_harmonic_in_band(f0, fs);  // 23
    const double ans_correct = spur_floor_exact(
        db, f0, fs, N, /*half_width=*/3, correct_max,
        /*odd_only=*/true);
    const double ans_overlarge = spur_floor_exact(
        db, f0, fs, N, /*half_width=*/3, /*max_harmonic=*/500,
        /*odd_only=*/true);
    CHECK(ans_correct == doctest::Approx(ans_overlarge).epsilon(1e-6));
    // Sanity: both read the synthetic spur floor.
    CHECK(ans_correct == doctest::Approx(-70.0).epsilon(0.5));
}

TEST_CASE("spur_floor_exact returns a safe sentinel on degenerate input") {
    std::vector<float> empty;
    CHECK(spur_floor_exact(empty, 1000.0, 48000, 8192, 3, 1, false)
          == doctest::Approx(-240.0));

    std::vector<float> one_bin{-60.0f};
    CHECK(spur_floor_exact(one_bin, 1000.0, 0, 8192, 3, 1, false)
          == doctest::Approx(-240.0));

    std::vector<float> short_spec{-60.0f};
    CHECK(spur_floor_exact(short_spec, 1000.0, 48000, 0, 3, 1, false)
          == doctest::Approx(-240.0));
}

TEST_CASE("goertzel_amplitude_db recovers a unit cosine's amplitude at "
          "off-grid frequencies (criterion 1 scalloping-free budget)") {
    // The whole point of Goertzel in D-012 item 4: measure an
    // off-grid tone's amplitude without FFT-bin scalloping bias.
    // A unit cosine at an off-grid frequency must still read
    // within ~0.05 dB of 0 dBFS -- well under SPIKE-3's 0.25 dB
    // cross-rate budget.
    constexpr std::size_t N = 8192;
    constexpr unsigned fs = 48000;

    // Sweep across a handful of off-grid frequencies in the audio
    // band.
    const double frequencies[] = {
        500.0, 997.3, 1000.549, 2500.7, 4321.25, 7999.9};
    for (double f0 : frequencies) {
        CAPTURE(f0);
        std::vector<float> x = make_cosine(f0, fs, N);
        const double db = goertzel_amplitude_db(x, f0, fs);
        CHECK(db == doctest::Approx(0.0).epsilon(0.1));
    }
}

TEST_CASE("goertzel_amplitude_db handles degenerate input gracefully") {
    std::vector<float> empty;
    CHECK(goertzel_amplitude_db(empty, 1000.0, 48000u)
          == doctest::Approx(-240.0));

    std::vector<float> single{0.0f};
    CHECK(goertzel_amplitude_db(single, 1000.0, 48000u)
          == doctest::Approx(-240.0));

    // Silence: -inf in theory, floored to ~-300 dBFS via the 1e-15
    // magnitude floor. Any very-negative dBFS result is acceptable.
    std::vector<float> silence(4096, 0.0f);
    CHECK(goertzel_amplitude_db(silence, 1000.0, 48000u) < -200.0);
}
