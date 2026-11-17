// src/spike/vcv_resampler/fft.h
//
// Hand-rolled radix-2 Cooley-Tukey FFT for the SPIKE-3 VCV resampler
// measurement harness (bead rackboy-d4y). Header-only, <80 lines of
// actual DFT code, zero third-party dependencies. Pulled into the
// spike rather than `rackboy_core` on purpose — this FFT is fine for
// "is this peak where I expect" measurement work, but it is NOT a
// production-grade DSP primitive. If the production resampler
// (rackboy-45x) ever needs an FFT, reach for PFFT, KissFFT, or one
// of the JUCE / liquid-dsp options and keep this spike-only code
// out of the shipping plugin.
//
// Scope:
//   * power-of-two sizes only
//   * single-precision floating point
//   * output is full complex spectrum (N/2 + 1 useful bins for a
//     real input — bins >= N/2 are the mirrored complex conjugate)
//   * no plan caching; re-initializes the twiddle table on every
//     call. Measurements run once per sample rate; a ~100 µs hit
//     per call is irrelevant next to the APU run time.
//
// Safety:
//   * no exceptions, no heap allocations on the analysis path
//     beyond the caller-provided in/out vectors
//   * `fft_forward` is `noexcept`
//
// Correctness reference: cross-checked against NumPy's
// `numpy.fft.fft` on known test vectors during spike development.

#ifndef RACKBOY_SPIKE_VCV_RESAMPLER_FFT_H_
#define RACKBOY_SPIKE_VCV_RESAMPLER_FFT_H_

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace rackboy::spike::vcv_resampler {

constexpr double kPi = 3.14159265358979323846;

// Bit-reverse permute `data` in place. `n` must be a power of 2.
// Used as the first pass of the Cooley-Tukey decimation-in-time FFT.
inline void bit_reverse_permute(std::vector<std::complex<float>>& data)
{
    const std::size_t n = data.size();
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
}

// In-place forward FFT. `n = data.size()` must be a power of 2 and
// >= 2. Output bin k (0 <= k < n) is the discrete Fourier transform
// at angular frequency `2*pi*k/n` rad/sample.
inline void fft_forward(std::vector<std::complex<float>>& data)
{
    const std::size_t n = data.size();
    if (n < 2) {
        return;
    }
    bit_reverse_permute(data);

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const std::complex<float> w_primitive(
            static_cast<float>(std::cos(angle)),
            static_cast<float>(std::sin(angle)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v =
                    data[i + k + len / 2] * w;
                data[i + k]             = u + v;
                data[i + k + len / 2]   = u - v;
                w *= w_primitive;
            }
        }
    }
}

// Apply a Hann window (length `n`) to `data` in place. The Hann
// window is the standard choice for quasi-sinusoidal spectral
// measurement: -31.5 dB sidelobe suppression, narrow-ish main
// lobe, integer power-of-two compatibility. Amplitude correction
// factor is 2.0 (add +6.02 dB to the measured peak in dBFS if you
// need un-windowed amplitude). Without a window, the spectral
// leakage from a non-integer-cycle sinusoid would drown out the
// -60 dBFS spur floor we care about.
inline void apply_hann_window(std::vector<float>& data) noexcept
{
    const std::size_t n = data.size();
    if (n < 2) {
        return;
    }
    const double denom = static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 * (1.0 - std::cos(
            2.0 * kPi * static_cast<double>(i) / denom));
        data[i] = static_cast<float>(data[i] * w);
    }
}

// Convert a full complex FFT into a magnitude-in-dBFS array of
// length n/2 + 1 (the useful real-input bins). Bin k corresponds
// to frequency `k * fs / n` Hz.
//
// Scaling derivation
// ------------------
// dBFS convention: 0 dBFS = unit-amplitude sinusoid (not unit DC).
// A unit DC input therefore reads -6.02 dBFS under this
// convention.
//
//   * Un-windowed FFT of a unit cosine → peak magnitude N/2
//     (amplitude split between positive and negative frequency
//     bins).
//   * Hann window has coherent gain 0.5 (the DC component of the
//     Hann sequence is 0.5), so a Hann-windowed unit cosine has
//     peak magnitude (N/2) * 0.5 = N/4.
//   * To map that peak to 1.0 → 0 dBFS, divide by N/4 — i.e.
//     multiply by 4/N.
//
// Earlier iterations of this helper used 2/N (halfway between the
// un-windowed and Hann-corrected cases), which produced dBFS
// numbers 6.02 dB below the correct value. See the iteration-65
// code review for the derivation; the scale factor below was
// corrected there.
//
// Bins below 1e-12 are floored to -240 dBFS to avoid -inf in plots.
inline std::vector<float> magnitude_db_fs(
    const std::vector<std::complex<float>>& spectrum) noexcept
{
    const std::size_t n = spectrum.size();
    const std::size_t half = n / 2 + 1;
    std::vector<float> out(half, 0.0f);
    const float scale = 4.0f / static_cast<float>(n);
    for (std::size_t k = 0; k < half; ++k) {
        const float mag = std::abs(spectrum[k]) * scale;
        const float mag_floor = mag < 1.0e-12f ? 1.0e-12f : mag;
        out[k] = 20.0f * std::log10(mag_floor);
    }
    return out;
}

// Return (bin_index, db_value) of the largest magnitude in
// `db_bins`, excluding DC (bin 0). Useful for locating the
// fundamental in a single-tone spectrum.
inline std::pair<std::size_t, float> peak_bin(
    const std::vector<float>& db_bins) noexcept
{
    std::size_t best_idx = 1;
    float best_db = db_bins.size() > 1 ? db_bins[1] : -240.0f;
    for (std::size_t k = 2; k < db_bins.size(); ++k) {
        if (db_bins[k] > best_db) {
            best_db = db_bins[k];
            best_idx = k;
        }
    }
    return {best_idx, best_db};
}

// Return the peak magnitude (dBFS) in `db_bins` among bins that
// are NOT within `exclude_half_width_bins` of a harmonic of
// `fundamental_bin`. Also excludes DC (bin 0). Used to measure the
// non-harmonic spur floor after identifying the fundamental.
//
// `odd_harmonics_only` controls which harmonics are excluded:
//
//   * `false` (default): exclude every integer multiple of the
//     fundamental up to `max_harmonic` — the conservative choice
//     for arbitrary stimuli.
//   * `true`: exclude only ODD multiples (1, 3, 5, …). Use this
//     for ideal 50%-duty square waves, whose Fourier series has
//     zero even-harmonic content by construction. Excluding the
//     even-multiple bins with `false` would silently hide any
//     legitimate non-harmonic spurs that land on an even-multiple
//     bin, underestimating the spur floor.
//
// A real Pulse1 output has tiny even-harmonic content from DAC
// nonlinearity and sampling jitter, but that content is typically
// 40+ dB below the odd-harmonic line — well below the -60 dBFS
// acceptance criterion — so including those bins in the spur floor
// is the honest reading.
//
// **Footgun (see `docs/DECISIONS.md` D-011).** `max_harmonic` must
// be >= floor(Nyquist / fundamental) for the result to be a *true*
// non-harmonic spur floor. If `max_harmonic` is smaller, legitimate
// in-band harmonics of the stimulus get counted as "non-harmonic
// spurs" and the return value lands at roughly
// `-20 * log10(max_harmonic + 2)` below the fundamental. The
// iteration-67 measurement bug called this helper with
// `max_harmonic = 20` against a stimulus whose in-band odd
// harmonics extended to harmonic 23 at 48 kHz Nyquist, which
// misclassified two legitimate signal harmonics as spurs and
// produced an artificially pessimistic -42 dBFS reading at 48 kHz
// (true value: -78 dBFS). Callers in tests should compute
// `max_harmonic = floor(Nyquist / fundamental)` rounded down to
// odd; `tests/resampler/spectral_test.cpp::max_odd_harmonic_in_band`
// is the canonical helper.
inline float non_harmonic_spur_floor_db(
    const std::vector<float>& db_bins,
    std::size_t fundamental_bin,
    std::size_t exclude_half_width_bins = 3,
    std::size_t max_harmonic = 20,
    bool odd_harmonics_only = false) noexcept
{
    float worst = -240.0f;
    for (std::size_t k = 1; k < db_bins.size(); ++k) {
        bool is_harmonic = false;
        for (std::size_t h = 1; h <= max_harmonic; ++h) {
            if (odd_harmonics_only && (h % 2) == 0) {
                continue;
            }
            const std::size_t harmonic_bin = fundamental_bin * h;
            const std::size_t lo =
                harmonic_bin > exclude_half_width_bins
                    ? harmonic_bin - exclude_half_width_bins
                    : 0;
            const std::size_t hi =
                harmonic_bin + exclude_half_width_bins;
            if (k >= lo && k <= hi) {
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

}  // namespace rackboy::spike::vcv_resampler

#endif  // RACKBOY_SPIKE_VCV_RESAMPLER_FFT_H_
