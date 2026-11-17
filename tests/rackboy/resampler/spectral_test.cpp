// tests/resampler/spectral_test.cpp
//
// Spectral-quality tests for the production resampler (bead
// rackboy-45x). Three cases:
//
//   1. "filter frequency response meets design spec" -- takes
//      the FFT of the decimator's impulse response (the filter
//      coefficients themselves, zero-padded) and asserts the
//      passband is flat and the stopband is suppressed. Independent
//      of any stimulus.
//
//   2. "decimator is transparent on a band-limited input" --
//      feeds a clean low-frequency sine through the decimator
//      and asserts the peak amplitude is preserved across rates
//      within the Hann-window scalloping-loss envelope (~1 dB).
//      Measurements use quadratic peak interpolation to remove
//      scalloping bias.
//
//   3. "APU -> decimator chain produces a bounded Pulse1
//      fundamental at every supported rate" -- asserts bounded
//      envelopes on peak frequency / amplitude / non-harmonic
//      spur floor at each of the four supported host rates.
//      48 kHz is expected to read below -60 dBFS by construction
//      (D-011 retracted the earlier "cycle-accumulator jitter"
//      diagnosis); 44.1 / 96 / 192 kHz still have real aliased-
//      harmonic content and fail SPIKE-3 criterion 2, which is
//      what `rackboy-efh` is scoped to fix.
//
// See docs/DECISIONS.md (D-010 retracted, D-011 current),
// docs/spikes/vcv-resampler.md, and
// docs/spikes/apu-cycle-accumulator-spurs.md for the measurement
// correction and the follow-up scope.

#include "doctest/doctest.h"

#include "rackboy_resampler.hpp"
#include "spike/vcv_resampler/fft.h"

extern "C" {
#include "rackboy_apu.h"
}

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using rackboy::resampler::Decimator;
using rackboy::resampler::decimator_init;
using rackboy::resampler::decimator_process;
using rackboy::resampler::kDecimationFactor;

namespace {

constexpr int kMeasureFftLength = 32768;
constexpr std::size_t kExclusionHalfWidthBins = 3;

// Upper harmonic index for the clean-sine test. A pure sine has
// no harmonics above the fundamental, so a small exclusion
// window is fine.
constexpr int kMaxHarmonicForCleanSine = 20;

// For a square-wave stimulus the spur-scan must exclude EVERY
// legitimate in-band odd harmonic of the fundamental, or the
// measurement misclassifies high harmonics as non-harmonic spurs
// (this was the iteration-67 measurement bug that led to the
// over-pessimistic D-010 diagnosis; see
// docs/spikes/apu-cycle-accumulator-spurs.md). Compute per-rate
// as floor(Nyquist / fundamental) rounded to odd.
inline int max_odd_harmonic_in_band(double fundamental_hz,
                                     unsigned sample_rate) noexcept {
    const double nyquist = static_cast<double>(sample_rate) * 0.5;
    // std::floor makes the rounding direction explicit: we want the
    // LARGEST integer <= Nyquist/f0 (not a naive truncation that
    // can behave differently for the negative inputs this helper
    // doesn't accept, but writing it this way documents the intent).
    int max_h = static_cast<int>(std::floor(nyquist / fundamental_hz));
    if ((max_h % 2) == 0) {
        max_h -= 1;
    }
    if (max_h < 1) {
        max_h = 1;
    }
    return max_h;
}

// Pulse1 stimulus parameters (SPIKE-3 pinning).
constexpr uint16_t kPulse1_N = 1917;
constexpr double kPulse1FreqHz =
    131072.0 / (2048.0 - static_cast<double>(kPulse1_N));

// --------------------------------------------------------------
// Quadratic peak interpolation.
// --------------------------------------------------------------
//
// Fit a parabola to three adjacent magnitude bins (in dB) and
// return the interpolated peak height and fractional offset.
// Removes Hann-window scalloping loss: a true 1 kHz sine reads
// within ~0.01 dB of its analytic amplitude even when the peak
// lies between bins.
struct PeakInterp {
    double peak_db;
    double peak_bin;
};

PeakInterp parabolic_peak(const std::vector<float>& db, std::size_t k) {
    if (k == 0 || k + 1 >= db.size()) {
        return {static_cast<double>(db[k]), static_cast<double>(k)};
    }
    const double a = db[k - 1];
    const double b = db[k];
    const double c = db[k + 1];
    const double denom = (a - 2.0 * b + c);
    if (denom == 0.0) {
        return {b, static_cast<double>(k)};
    }
    const double delta = 0.5 * (a - c) / denom;
    const double peak_db = b - 0.25 * (a - c) * delta;
    return {peak_db, static_cast<double>(k) + delta};
}

// --------------------------------------------------------------
// Case 1 helper: FFT of the decimator filter itself.
// --------------------------------------------------------------
//
// Zero-pad the filter impulse response to 4096 samples (so the
// frequency grid is fine enough to see ripple and transition
// band) and take the FFT. Returns magnitude in dBFS relative to
// the 0 Hz bin (i.e., normalised so DC gain reads 0 dB exactly).
std::vector<float> filter_frequency_response(const Decimator& d) {
    constexpr int kFftLen = 4096;
    REQUIRE(d.filter_length <= kFftLen);
    std::vector<std::complex<float>> buf(
        static_cast<std::size_t>(kFftLen),
        std::complex<float>(0.0f, 0.0f));
    for (int i = 0; i < d.filter_length; ++i) {
        buf[static_cast<std::size_t>(i)] =
            std::complex<float>(d.coeffs[i], 0.0f);
    }
    rackboy::spike::vcv_resampler::fft_forward(buf);
    std::vector<float> db(static_cast<std::size_t>(kFftLen / 2 + 1), 0.0f);
    const float dc = std::abs(buf[0]);
    REQUIRE(dc > 0.0f);
    for (int k = 0; k <= kFftLen / 2; ++k) {
        const float mag = std::abs(buf[static_cast<std::size_t>(k)]) / dc;
        const float mag_floor = mag < 1.0e-12f ? 1.0e-12f : mag;
        db[static_cast<std::size_t>(k)] = 20.0f * std::log10(mag_floor);
    }
    return db;
}

// --------------------------------------------------------------
// Case 2 helper: render a clean sine through the decimator.
// --------------------------------------------------------------

std::vector<float> render_clean_sine_host(unsigned host_rate,
                                           double sine_hz,
                                           double amplitude,
                                           int out_count) {
    const unsigned internal_rate =
        host_rate * static_cast<unsigned>(kDecimationFactor);
    Decimator d{};
    REQUIRE(decimator_init(&d, host_rate));

    constexpr int kApuBlock = 512;
    static_assert(kApuBlock % kDecimationFactor == 0, "");
    const int kHostBlock = kApuBlock / kDecimationFactor;
    std::vector<float> in1(kApuBlock), in2(kApuBlock, 0.0f),
                       in3(kApuBlock, 0.0f), in4(kApuBlock, 0.0f);
    std::vector<float> o1(kHostBlock), o2(kHostBlock),
                       o3(kHostBlock), o4(kHostBlock);

    const int warmup_host =
        (d.filter_length / kDecimationFactor) + 8;

    std::vector<float> collected;
    collected.reserve(static_cast<std::size_t>(out_count));

    int host_written = 0;
    uint64_t sample_index = 0;
    const double two_pi = 2.0 * 3.14159265358979323846;
    const double w = two_pi * sine_hz / static_cast<double>(internal_rate);

    while (host_written < warmup_host + out_count) {
        for (int i = 0; i < kApuBlock; ++i) {
            const double phase =
                w * static_cast<double>(sample_index + static_cast<uint64_t>(i));
            in1[static_cast<std::size_t>(i)] =
                static_cast<float>(amplitude * std::sin(phase));
        }
        sample_index += static_cast<uint64_t>(kApuBlock);

        const int n = decimator_process(
            &d, in1.data(), in2.data(), in3.data(), in4.data(),
            kApuBlock,
            o1.data(), o2.data(), o3.data(), o4.data());
        REQUIRE(n == kHostBlock);

        for (int i = 0; i < n; ++i) {
            if (host_written >= warmup_host &&
                static_cast<int>(collected.size()) < out_count) {
                collected.push_back(o1[static_cast<std::size_t>(i)]);
            }
            ++host_written;
        }
    }
    REQUIRE(static_cast<int>(collected.size()) == out_count);
    return collected;
}

// --------------------------------------------------------------
// Case 3 helper: APU Pulse1 through decimator.
// --------------------------------------------------------------

void configure_pulse1(rackboy_apu_t* apu) {
    rackboy_apu_write_reg(apu, 0xFF26, 0x80);
    rackboy_apu_write_reg(apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(apu, 0xFF10, 0x00);
    rackboy_apu_write_reg(apu, 0xFF11, 0x80);
    rackboy_apu_write_reg(apu, 0xFF12, 0xF0);
    rackboy_apu_write_reg(apu, 0xFF13, static_cast<uint8_t>(kPulse1_N & 0xFF));
    rackboy_apu_write_reg(apu, 0xFF14,
                          static_cast<uint8_t>(0x80 | ((kPulse1_N >> 8) & 0x07)));
}

std::vector<float> render_pulse1_through_decimator(unsigned host_rate,
                                                    int out_count) {
    const unsigned internal_rate =
        host_rate * static_cast<unsigned>(kDecimationFactor);
    rackboy_apu_t apu;
    rackboy_apu_init(&apu, internal_rate);
    configure_pulse1(&apu);

    Decimator d{};
    REQUIRE(decimator_init(&d, host_rate));

    constexpr int kApuBlock = 512;
    static_assert(kApuBlock % kDecimationFactor == 0, "");
    const int kHostBlock = kApuBlock / kDecimationFactor;
    std::vector<float> in1(kApuBlock), in2(kApuBlock),
                       in3(kApuBlock), in4(kApuBlock);
    std::vector<float> o1(kHostBlock), o2(kHostBlock),
                       o3(kHostBlock), o4(kHostBlock);

    const int warmup_host =
        static_cast<int>(host_rate / 10)
        + (d.filter_length / kDecimationFactor) + 4;

    std::vector<float> collected;
    collected.reserve(static_cast<std::size_t>(out_count));

    int host_written = 0;
    while (host_written < warmup_host + out_count) {
        rackboy_apu_run_samples(&apu, static_cast<uint32_t>(kApuBlock),
                                in1.data(), in2.data(), in3.data(), in4.data());
        const int n = decimator_process(
            &d, in1.data(), in2.data(), in3.data(), in4.data(), kApuBlock,
            o1.data(), o2.data(), o3.data(), o4.data());
        REQUIRE(n == kHostBlock);

        for (int i = 0; i < n; ++i) {
            if (host_written >= warmup_host &&
                static_cast<int>(collected.size()) < out_count) {
                collected.push_back(o1[static_cast<std::size_t>(i)]);
            }
            ++host_written;
        }
    }
    REQUIRE(static_cast<int>(collected.size()) == out_count);
    return collected;
}

struct Measurement {
    float peak_db;
    float spur_floor_db;
    double peak_freq_hz;
};

Measurement measure_odd(const std::vector<float>& samples, unsigned fs) {
    using rackboy::spike::vcv_resampler::apply_hann_window;
    using rackboy::spike::vcv_resampler::fft_forward;
    using rackboy::spike::vcv_resampler::magnitude_db_fs;
    using rackboy::spike::vcv_resampler::peak_bin;
    using rackboy::spike::vcv_resampler::non_harmonic_spur_floor_db;

    REQUIRE(static_cast<int>(samples.size()) >= kMeasureFftLength);
    std::vector<float> block(samples.begin(),
                             samples.begin() + kMeasureFftLength);
    apply_hann_window(block);
    std::vector<std::complex<float>> spec(kMeasureFftLength);
    for (int i = 0; i < kMeasureFftLength; ++i) {
        spec[static_cast<std::size_t>(i)] =
            std::complex<float>(block[static_cast<std::size_t>(i)], 0.0f);
    }
    fft_forward(spec);
    const auto db = magnitude_db_fs(spec);
    const auto peak = peak_bin(db);
    // Cover every in-band odd harmonic of the Pulse1 fundamental
    // so high harmonics are NOT misclassified as non-harmonic
    // spurs (the iteration-67 measurement bug).
    const int max_h = max_odd_harmonic_in_band(kPulse1FreqHz, fs);
    const float spur = non_harmonic_spur_floor_db(
        db, peak.first, kExclusionHalfWidthBins,
        static_cast<std::size_t>(max_h),
        /*odd_harmonics_only=*/true);
    const double bin_hz =
        static_cast<double>(fs) / static_cast<double>(kMeasureFftLength);
    return {peak.second, spur,
            static_cast<double>(peak.first) * bin_hz};
}

}  // namespace

TEST_CASE("max_odd_harmonic_in_band returns the right exclusion depth") {
    // This helper encodes the iteration-68 correction: it must
    // round DOWN to the largest odd integer <= Nyquist/f0 at
    // each supported rate. A bug here would silently re-enable
    // the D-010 measurement artefact.
    CHECK(max_odd_harmonic_in_band(kPulse1FreqHz, 44100) == 21);
    CHECK(max_odd_harmonic_in_band(kPulse1FreqHz, 48000) == 23);
    CHECK(max_odd_harmonic_in_band(kPulse1FreqHz, 96000) == 47);
    CHECK(max_odd_harmonic_in_band(kPulse1FreqHz, 192000) == 95);

    // Edge: fundamental above Nyquist produces max_h = 1 (safe
    // floor, never non-positive).
    CHECK(max_odd_harmonic_in_band(30000.0, 48000) == 1);
    CHECK(max_odd_harmonic_in_band(25000.0, 48000) == 1);

    // Edge: floor(Nyquist/f0) is even -> step down to odd.
    // 48000 / (2 * 1200) = 20 (even) -> expect 19.
    CHECK(max_odd_harmonic_in_band(1200.0, 48000) == 19);
}

TEST_CASE("decimator filter meets its passband and stopband design spec") {
    Decimator d{};
    REQUIRE(decimator_init(&d, 48000));
    const auto db = filter_frequency_response(d);
    // FFT length 4096, internal rate = 192000. Bin spacing is
    // 192000 / 4096 = 46.875 Hz (relative to internal rate). In
    // normalised cycles/sample (re. internal rate) that is
    // 1 / 4096 per bin.
    // Passband edge: 0.45 * host / internal = 0.1125 cyc/sample.
    //   bin index: 0.1125 * 4096 = 460.8 -> floor to 460.
    // Stopband edge: 0.50 * host / internal = 0.1250 cyc/sample.
    //   bin index: 0.1250 * 4096 = 512.

    const int passband_hi = 455;  // slight margin inside 460.8
    const int stopband_lo = 516;  // slight margin beyond 512
    const int kHalfFft = 4096 / 2;

    // Passband ripple: the magnitude response must stay within
    // 0.1 dB of DC gain (0 dB) from 0 to the passband edge.
    // Kaiser windows with matched beta/N overshoot in the
    // passband is well below 0.1 dB for -80 dB stopband.
    float max_ripple = 0.0f;
    for (int k = 0; k <= passband_hi; ++k) {
        const float v = db[static_cast<std::size_t>(k)];
        const float ripple = std::fabs(v);
        if (ripple > max_ripple) {
            max_ripple = ripple;
        }
    }
    CAPTURE(max_ripple);
    CHECK(max_ripple <= 0.1f);

    // Stopband attenuation: every bin from stopband_lo to
    // Nyquist must be at least 70 dB below DC. We designed for
    // 80 dB so 70 leaves clear margin.
    float worst_stop = -240.0f;
    for (int k = stopband_lo; k <= kHalfFft; ++k) {
        const float v = db[static_cast<std::size_t>(k)];
        if (v > worst_stop) {
            worst_stop = v;
        }
    }
    CAPTURE(worst_stop);
    CHECK(worst_stop <= -70.0f);
}

TEST_CASE("decimator is transparent on a band-limited input") {
    // Feed a pure sine close to 1 kHz through the decimator at
    // each supported rate. To eliminate Hann scalloping bias
    // we choose a rate-dependent frequency that lands exactly on
    // an FFT bin centre: frequency = round(1000 * N / fs) * fs / N,
    // where N is the FFT length. The measured peak at every rate
    // should match the analytic input amplitude within 0.05 dB.
    const unsigned rates[] = {44100, 48000, 96000, 192000};
    constexpr double kAmp = 0.15;
    const double analytic_peak_db = 20.0 * std::log10(kAmp);
    std::vector<double> interp_peaks;
    interp_peaks.reserve(4);

    for (const unsigned rate : rates) {
        CAPTURE(rate);
        const double bin_hz =
            static_cast<double>(rate) / static_cast<double>(kMeasureFftLength);
        const int target_bin = static_cast<int>(std::round(1000.0 / bin_hz));
        const double sine_hz =
            static_cast<double>(target_bin) * bin_hz;
        CAPTURE(target_bin);
        CAPTURE(sine_hz);
        const int measure_samples = static_cast<int>(rate);
        const auto buf = render_clean_sine_host(rate, sine_hz, kAmp,
                                                measure_samples);

        using rackboy::spike::vcv_resampler::apply_hann_window;
        using rackboy::spike::vcv_resampler::fft_forward;
        using rackboy::spike::vcv_resampler::magnitude_db_fs;
        using rackboy::spike::vcv_resampler::peak_bin;
        using rackboy::spike::vcv_resampler::non_harmonic_spur_floor_db;

        std::vector<float> block(buf.begin(),
                                 buf.begin() + kMeasureFftLength);
        apply_hann_window(block);
        std::vector<std::complex<float>> spec(kMeasureFftLength);
        for (int i = 0; i < kMeasureFftLength; ++i) {
            spec[static_cast<std::size_t>(i)] =
                std::complex<float>(block[static_cast<std::size_t>(i)], 0.0f);
        }
        fft_forward(spec);
        const auto db = magnitude_db_fs(spec);
        const auto raw_peak = peak_bin(db);
        const auto interp = parabolic_peak(db, raw_peak.first);
        CAPTURE(raw_peak.second);
        CAPTURE(interp.peak_db);

        // After interpolation, peak should match analytic within
        // ~0.05 dB (float FFT + windowing noise).
        CHECK(std::fabs(interp.peak_db - analytic_peak_db) <= 0.05);

        // Spur floor (all-harmonic exclusion since input is a
        // pure sine) must be at least -85 dBFS -- the decimator
        // must not inject measurable spurs. Empirically the
        // float-FFT noise floor hovers near -93 dBFS here, well
        // below the SPIKE-3 -60 dBFS criterion (25 dB headroom).
        const float spur = non_harmonic_spur_floor_db(
            db, raw_peak.first, kExclusionHalfWidthBins,
            static_cast<std::size_t>(kMaxHarmonicForCleanSine),
            /*odd_harmonics_only=*/false);
        CAPTURE(spur);
        CHECK(spur <= -85.0f);

        interp_peaks.push_back(interp.peak_db);
    }

    // Inter-rate spread after interpolation -- should be <<
    // SPIKE-3's 0.25 dB binding for a truly band-limited input.
    const double max_db = *std::max_element(interp_peaks.begin(),
                                            interp_peaks.end());
    const double min_db = *std::min_element(interp_peaks.begin(),
                                            interp_peaks.end());
    const double spread = max_db - min_db;
    CAPTURE(min_db);
    CAPTURE(max_db);
    CAPTURE(spread);
    CHECK(spread <= 0.05);
}

TEST_CASE("APU -> decimator chain produces a bounded Pulse1 "
          "fundamental at every supported rate") {
    // Bounded envelope around the APU-path measurements at each
    // supported host rate, with per-rate `max_harmonic`
    // exclusion (see max_odd_harmonic_in_band above) so the
    // spur-floor number is a true non-harmonic floor rather
    // than the iteration-67 artefact that mis-classified
    // harmonics 21 and 23 as spurs at 48 kHz.
    //
    // Envelope widths (~5 dB either side on the failing rates,
    // ~25 dB on 48 kHz's headroom) absorb float-FFT noise and
    // incidental compiler / libm shifts while staying tight
    // enough that a regression in the decimator or APU -- a
    // muted channel, a phase inversion, a spur-floor doubling,
    // or a peak-frequency drift greater than a few FFT bins --
    // fails the build. If a follow-up change (e.g. rackboy-efh
    // landing an upstream fix) legitimately moves a number past
    // its envelope, the bound for that rate must be regenerated
    // in the same commit. Do NOT silently widen: surfacing a
    // real improvement is the point of having the bound.
    struct Expected {
        unsigned rate;
        double   peak_freq_min_hz;
        double   peak_freq_max_hz;
        float    peak_db_min;
        float    peak_db_max;
        float    spur_db_min;
        float    spur_db_max;
    };
    // Pulse1 fundamental is 131072 / (2048 - 1917) ~= 1000.549 Hz.
    // Measured peak frequencies across rates fall within ~2 FFT
    // bins of that (scalloping + accumulator phase), measured
    // peak amplitudes sit near -17 to -16 dBFS, measured spur
    // floors sit in the range -43 to -35 dBFS per the SPIKE-3
    // baseline and the iteration-67 parametric sweep.
    // Measured envelope under the corrected max_odd_harmonic_in_band
    // exclusion (see apu-cycle-accumulator-spurs spike doc). Note
    // that 48 kHz now reads dramatically cleaner than the rest --
    // the measurement bug in iteration-67 falsely grouped it with
    // the other rates at ~-42 dBFS. The legitimate spur content
    // at 44.1 / 96 / 192 kHz comes from aliased Pulse1 harmonics
    // folding onto non-harmonic bins; those rates still need the
    // upstream fix tracked by rackboy-efh.
    const Expected expected[] = {
        {44100,  998.0, 1002.0, -18.0f, -16.0f, -38.0f, -32.0f},
        {48000,  998.0, 1003.0, -17.0f, -15.0f, -100.0f, -65.0f},
        {96000, 1000.0, 1003.0, -18.0f, -16.0f, -40.0f, -32.0f},
        {192000, 1000.0, 1003.0, -17.0f, -15.0f, -45.0f, -37.0f},
    };

    std::vector<float> peak_dbs;
    peak_dbs.reserve(sizeof(expected) / sizeof(expected[0]));

    for (const auto& e : expected) {
        CAPTURE(e.rate);
        const int measure_samples = static_cast<int>(e.rate);
        const auto buf = render_pulse1_through_decimator(e.rate,
                                                         measure_samples);
        const auto m = measure_odd(buf, e.rate);
        CAPTURE(m.peak_db);
        CAPTURE(m.spur_floor_db);
        CAPTURE(m.peak_freq_hz);

        CHECK(m.peak_freq_hz >= e.peak_freq_min_hz);
        CHECK(m.peak_freq_hz <= e.peak_freq_max_hz);
        CHECK(m.peak_db >= e.peak_db_min);
        CHECK(m.peak_db <= e.peak_db_max);
        CHECK(m.spur_floor_db >= e.spur_db_min);
        CHECK(m.spur_floor_db <= e.spur_db_max);

        peak_dbs.push_back(m.peak_db);
    }

    // Inter-rate peak spread envelope. The direct-rate SPIKE-3
    // baseline measured 1.30 dB of spread; a regression that
    // changed this drastically (either way) is worth surfacing.
    const float max_db = *std::max_element(peak_dbs.begin(),
                                           peak_dbs.end());
    const float min_db = *std::min_element(peak_dbs.begin(),
                                           peak_dbs.end());
    const float spread = max_db - min_db;
    CAPTURE(min_db);
    CAPTURE(max_db);
    CAPTURE(spread);
    CHECK(spread >= 0.5f);   // non-zero spread confirms per-rate variance
    CHECK(spread <= 2.5f);   // bounded so silent drift is caught
}
