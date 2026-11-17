// tests/resampler/measurement.hpp
//
// Spectral measurement helpers promoted from
// src/spike/apu_bandlimited/main.cpp per docs/DECISIONS.md D-012
// item 4 and bead rackboy-dmq.
//
// Header-only. Test-only (never linked into the VCV plugin or the
// Daisy firmware): these helpers are what the production resampler
// spectral acceptance test in tests/resampler/host_resampler_test.cpp
// uses to assert SPIKE-3 criteria end-to-end:
//
//   * Criterion 1: inter-rate fundamental peak spread <= 0.25 dB.
//     Measured via Goertzel at the exact stimulus frequency (no
//     FFT scalloping, no window bias).
//   * Criterion 2: non-harmonic spur floor <= -60 dBFS at every
//     supported host rate. Measured via a Blackman-Harris 4-term
//     windowed FFT (-92 dB sidelobes so legitimate harmonic leakage
//     does not mask the spur floor) plus exact-frequency harmonic
//     exclusion (`h * f0 * N_fft / fs`, NOT `fundamental_bin * h`).
//
// Why these specific three helpers:
//
//   * Hann + bin-peak scanning is only good enough for a ~1 dB
//     smoke regression. SPIKE-3 measured the bandlimited pipeline
//     at 92 dB under the -60 dBFS criterion; tightening the
//     production test to 0.25 / -60 requires analysis methods
//     that don't add their own ~1 dB of window-induced bias.
//   * Without exact-frequency harmonic exclusion, an off-grid
//     fundamental at 1000.55 Hz at 96 kHz with a 32768-point FFT
//     has its 23rd harmonic land ~10 bins off from
//     `fundamental_bin * 23`; a +/- 3 bin exclusion window misses
//     it and the "spur floor" reads the legitimate harmonic as a
//     spur. See spike comment at src/spike/apu_bandlimited/main.cpp
//     line 215 for the original investigation.
//
// Consumers should include this header + spike/vcv_resampler/fft.h
// (for fft_forward) and drive the resampler end-to-end through the
// same APU-at-48k stimulus the spike uses.

#ifndef RACKBOY_TESTS_RESAMPLER_MEASUREMENT_HPP_
#define RACKBOY_TESTS_RESAMPLER_MEASUREMENT_HPP_

#include "spike/vcv_resampler/fft.h"

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace rackboy {
namespace resampler {
namespace measurement {

// Blackman-Harris 4-term window coefficients per Harris 1978.
// Coherent gain CG = a0 = 0.35875. Highest sidelobe suppression
// 92 dB below main-lobe peak.
constexpr double kBH4_a0 = 0.35875;
constexpr double kBH4_a1 = 0.48829;
constexpr double kBH4_a2 = 0.14128;
constexpr double kBH4_a3 = 0.01168;

// Apply Blackman-Harris 4-term window to `data` in place. Length
// must be >= 2; shorter inputs return unchanged (matches the
// Hann window helper's convention in spike/vcv_resampler/fft.h).
//
// Amplitude correction factor for a unit-cosine -> 0 dBFS read is
// 2 / (CG * N) = 2 / (a0 * N) (see bh4_magnitude_db_fs below).
inline void apply_bh4_window(std::vector<float>& data) noexcept {
    const std::size_t n = data.size();
    if (n < 2) { return; }
    const double denom = static_cast<double>(n - 1);
    const double two_pi =
        2.0 * rackboy::spike::vcv_resampler::kPi;
    for (std::size_t i = 0; i < n; ++i) {
        const double k = static_cast<double>(i) / denom;
        const double w = kBH4_a0
                       - kBH4_a1 * std::cos(two_pi * k)
                       + kBH4_a2 * std::cos(2.0 * two_pi * k)
                       - kBH4_a3 * std::cos(3.0 * two_pi * k);
        data[i] = static_cast<float>(data[i] * w);
    }
}

// Return the dBFS magnitude spectrum (half-spectrum, length n/2+1)
// from a full complex FFT output, BH4-corrected so a unit-amplitude
// cosine peaks at 0 dBFS.
//
// Scale derivation (mirrors magnitude_db_fs in fft.h, with BH4's
// CG substituted for Hann's 0.5):
//   * Un-windowed FFT of a unit cosine -> peak |X[k]| = N/2.
//   * BH4 has CG = a0 = 0.35875, so the windowed peak is
//     (N/2) * a0 = N * a0 / 2.
//   * Mapping that peak to 1.0 = 0 dBFS requires scale =
//     2 / (a0 * N).
//
// Magnitudes below 1e-12 are floored to -240 dBFS so log10 does
// not emit -inf on plot data near silence.
inline std::vector<float> bh4_magnitude_db_fs(
        const std::vector<std::complex<float>>& spectrum) noexcept {
    const std::size_t n = spectrum.size();
    const std::size_t half = n / 2 + 1;
    std::vector<float> out(half, 0.0f);
    const float scale =
        static_cast<float>(2.0 / (kBH4_a0 * static_cast<double>(n)));
    for (std::size_t k = 0; k < half; ++k) {
        const float mag = std::abs(spectrum[k]) * scale;
        const float mag_floor = mag < 1.0e-12f ? 1.0e-12f : mag;
        out[k] = 20.0f * std::log10(mag_floor);
    }
    return out;
}

// Maximum integer harmonic of `f0` that fits under the Nyquist
// frequency at sampling rate `fs`. Returns at least 1.
inline int max_harmonic_in_band(double f0, unsigned fs) noexcept {
    if (f0 <= 0.0 || fs == 0) { return 1; }
    const double nyquist = static_cast<double>(fs) * 0.5;
    int max_h = static_cast<int>(std::floor(nyquist / f0));
    if (max_h < 1) { max_h = 1; }
    return max_h;
}

// Same as `max_harmonic_in_band` but clamps to the largest ODD
// harmonic at or below the Nyquist. Useful for pure-square-wave
// stimuli like the APU Pulse1 test tone, which has only odd
// harmonics (plus the fundamental) by symmetry.
inline int max_odd_harmonic_in_band(double f0, unsigned fs) noexcept {
    int max_h = max_harmonic_in_band(f0, fs);
    if ((max_h % 2) == 0) { --max_h; }
    if (max_h < 1) { max_h = 1; }
    return max_h;
}

// Return the worst non-harmonic spur in `db_bins` (a half-spectrum
// produced by `bh4_magnitude_db_fs` or `magnitude_db_fs`). Harmonic
// frequencies are computed EXACTLY as `h * f0 * N_fft / fs` and
// excluded with a +/- `half_width_bins` window around each. Bin 0
// (DC) is always excluded.
//
// Why exact-frequency exclusion: an off-grid fundamental at
// (say) 1000.55 Hz at fs = 96 kHz / N_fft = 32768 places harmonic
// h = 23 at bin ~7847, while `peak_bin * 23` lands ~10 bins off.
// A 3-bin half-width window misses the true harmonic and counts it
// as a spur. `h * f0 * N_fft / fs` is correct to within rounding.
//
// `max_harmonic` caps the iteration and MUST satisfy
// `max_harmonic <= floor(Nyquist / f0)` -- the canonical source is
// `max_harmonic_in_band(f0, fs)` for the all-harmonics case or
// `max_odd_harmonic_in_band(f0, fs)` for the odd-only case. Passing
// a value above Nyquist is not a crash, but the over-band harmonics
// silently land on bins outside the spectrum and are effectively
// ignored -- the result is still well-defined, just wastes work.
// `odd_harmonics_only = true` limits exclusion to odd harmonics
// (h = 1, 3, 5, ...), which includes the fundamental. Appropriate
// for pure square-wave stimuli (Pulse1 at 50%% duty) that have no
// even-harmonic energy; even harmonics (if present) are legit spurs
// and should NOT be excluded.
//
// `fft_length` is the length of the original FFT (i.e. what was
// passed to fft_forward, which is 2 * (half - 1) or 2 * half - 2
// for a real-input spectrum of length `half`). We require the
// caller to pass it rather than re-deriving, because a callsite
// that fed a zero-padded FFT still wants its *logical* length for
// the harmonic-bin math.
inline double spur_floor_exact(const std::vector<float>& db_bins,
                               double fundamental_hz,
                               unsigned fs,
                               std::size_t fft_length,
                               std::size_t half_width_bins,
                               int max_harmonic,
                               bool odd_harmonics_only) noexcept {
    if (db_bins.empty() || fft_length == 0 || fs == 0) {
        return -240.0;
    }
    const double bin_hz =
        static_cast<double>(fs) / static_cast<double>(fft_length);
    double worst = -240.0;
    for (std::size_t k = 1; k < db_bins.size(); ++k) {
        bool is_harmonic = false;
        for (int h = 1; h <= max_harmonic; ++h) {
            if (odd_harmonics_only && (h % 2) == 0) { continue; }
            const double harmonic_bin_exact =
                static_cast<double>(h) * fundamental_hz / bin_hz;
            const double distance =
                std::abs(static_cast<double>(k) - harmonic_bin_exact);
            if (distance <= static_cast<double>(half_width_bins)) {
                is_harmonic = true;
                break;
            }
        }
        if (!is_harmonic && db_bins[k] > worst) {
            worst = db_bins[k];
        }
    }
    return worst;
}

// Goertzel algorithm: amplitude (in dBFS) of a tone at exact
// frequency `f_hz` in the signal `x` (sampled at `fs` Hz).
// Hann-windowed internally so the dBFS number is directly
// comparable against a Hann-windowed FFT peak; the 4/N scaling
// matches `magnitude_db_fs` in spike/vcv_resampler/fft.h.
//
// Goertzel is chosen over FFT-peak interpolation for criterion 1:
// on an off-grid fundamental it is scalloping-free (evaluates the
// DTFT at exactly `f_hz`), so the 0.25 dB inter-rate spread budget
// is not spent on Hann bin-edge bias.
//
// Returns `-240 dBFS` for a degenerate input (length < 2). Floors
// the magnitude at 1e-15 to avoid log10 emitting -inf for exact
// silence.
inline double goertzel_amplitude_db(const std::vector<float>& x,
                                    double f_hz,
                                    unsigned fs) noexcept {
    const std::size_t N = x.size();
    if (N < 2) { return -240.0; }

    std::vector<double> windowed(N);
    const double denom = static_cast<double>(N - 1);
    const double pi = rackboy::spike::vcv_resampler::kPi;
    for (std::size_t i = 0; i < N; ++i) {
        const double w = 0.5 * (1.0 - std::cos(
            2.0 * pi * static_cast<double>(i) / denom));
        windowed[i] = static_cast<double>(x[i]) * w;
    }

    // Evaluate at the fractional bin `k_frac` via the standard
    // complex-pole recursion. `omega = 2*pi*k_frac/N` is the
    // exact analogue angular frequency of `f_hz` at `fs` Hz.
    const double k_frac =
        static_cast<double>(N) * f_hz / static_cast<double>(fs);
    const double omega =
        2.0 * pi * k_frac / static_cast<double>(N);
    const double coeff = 2.0 * std::cos(omega);

    double s_prev  = 0.0;
    double s_prev2 = 0.0;
    for (std::size_t n = 0; n < N; ++n) {
        const double s = windowed[n] + coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev  = s;
    }
    const double real_part = s_prev - s_prev2 * std::cos(omega);
    const double imag_part = s_prev2 * std::sin(omega);
    const double mag = std::sqrt(real_part * real_part +
                                 imag_part * imag_part);

    // 4/N so a unit-amplitude cosine after the Hann window reads
    // 0 dBFS (matches magnitude_db_fs's scaling).
    const double scale = 4.0 / static_cast<double>(N);
    const double mag_scaled = mag * scale;
    const double floor_val =
        mag_scaled < 1.0e-15 ? 1.0e-15 : mag_scaled;
    return 20.0 * std::log10(floor_val);
}

}  // namespace measurement
}  // namespace resampler
}  // namespace rackboy

#endif  // RACKBOY_TESTS_RESAMPLER_MEASUREMENT_HPP_
