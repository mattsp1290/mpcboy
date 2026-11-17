// src/rackboy_resampler.cpp
//
// Production resampler implementation (bead rackboy-45x). See
// rackboy_resampler.hpp for the interface contract and design
// rationale.
//
// Filter design summary
//
//   Target host rates: {44100, 48000, 96000, 192000} Hz.
//   Internal rate:      4 x host.
//   Passband edge:      0.45 x host_rate     (e.g. 21.6 kHz @ 48k)
//   Stopband edge:      0.50 x host_rate     (e.g. 24.0 kHz @ 48k)
//   Normalised (per internal rate):
//     wp = 0.1125 cycles / sample
//     ws = 0.1250 cycles / sample
//     transition width dw = 0.0125 cycles / sample
//   Stopband attenuation: >= 80 dB
//
//   Kaiser beta (Oppenheim & Schafer, eq. 7.73):
//     A >= 50: beta = 0.1102 (A - 8.7)   ->   beta ~= 7.857 at 80 dB
//
//   Filter length (Oppenheim & Schafer, eq. 7.75), with dw in
//   radians/sample so dw_rad = 2*pi * dw_cycles = 0.0125 * 2*pi
//   ~= 0.07854:
//     N_raw = ceil( (A - 8) / (2.285 * dw_rad) )
//           = ceil( 72 / 0.17945... )
//           = ceil( 401.277... )
//           = 402
//     N_odd = round_up_odd(402) = 403
//
//   The shipped filter is therefore 403 taps at every supported
//   host rate (the normalisation is defined relative to the internal
//   rate, not the host rate). 403 taps yields A ~= 80 dB stopband
//   attenuation across the 0.45..0.5 x host-rate transition band
//   with < 0.1 dB passband ripple, verified in
//   tests/resampler/spectral_test.cpp.
//
// DC gain is normalised to 1.0 at design time; the test suite
// asserts sum(coeffs) within a few ULPs of 1.0. All tap
// computation is in double precision, rounded to float at the
// last step to keep coefficient error below 1 ULP float.

#include "rackboy_resampler.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace rackboy {
namespace resampler {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Anti-denormal DC bias. The decimator's `double` accumulator
// avoids denormals during the dot-product sum itself (double has
// enough exponent headroom), but the final cast back to float in
// decimator_process() can still produce denormal outputs on long
// silence tails, which downstream consumers would then pay a
// 10-100x slowdown on every subsequent multiply. Biasing the
// accumulator by a value that rounds away under the cast defeats
// that without perceptibly shifting the output. kDenormalBias
// ~= 1e-20 is below every float's denormal threshold (1.4e-45
// float min, 1.2e-38 float_min_normal); it cancels to zero after
// the `+bias - bias` idiom at float precision.
constexpr double kDenormalBias = 1e-20;

// --------------------------------------------------------------
// Kaiser-window helpers
// --------------------------------------------------------------

// Modified Bessel function of the first kind, order zero.
// Series sum I0(x) = sum_{k=0..inf} ((x/2)^(2k) / (k!)^2).
// Converges very fast for the x ranges we need (|x| <= ~9 here;
// beta * sqrt(1 - (2n/(N-1) - 1)^2) <= beta <= ~9). Hard-stop at
// 50 terms so the loop is statically bounded.
inline double bessel_i0(double x) noexcept {
    const double half = x * 0.5;
    double term = 1.0;   // k = 0 term
    double sum = 1.0;
    for (int k = 1; k < 50; ++k) {
        // term_k = term_{k-1} * (x/2)^2 / k^2
        term *= (half * half) / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

// Kaiser beta from stopband attenuation A (dB), per Oppenheim &
// Schafer eq. 7.73.
inline double kaiser_beta(double A) noexcept {
    if (A > 50.0) {
        return 0.1102 * (A - 8.7);
    } else if (A >= 21.0) {
        return 0.5842 * std::pow(A - 21.0, 0.4) + 0.07886 * (A - 21.0);
    }
    return 0.0;
}

// Sinc function: sinc(x) = sin(pi*x) / (pi*x), sinc(0) = 1.
inline double sinc_pi(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }
    const double pi_x = kPi * x;
    return std::sin(pi_x) / pi_x;
}

// --------------------------------------------------------------
// Filter design
// --------------------------------------------------------------
//
// Designs a symmetric (linear-phase Type-I) low-pass FIR of
// length `N` (must be odd) with cutoff `fc` (cycles/sample; the
// -6 dB point) windowed by a Kaiser of shape `beta`. Writes
// taps into `out` and returns sum(taps) (pre-normalisation).
//
// Reference: Oppenheim & Schafer, DTSP 3e, ch. 7.
double design_kaiser_lowpass(int N, double fc, double beta,
                             double* out /* [N] */) noexcept {
    // N should be odd, positive, and within bounds.
    if (N <= 0 || (N % 2) == 0) {
        return 0.0;
    }
    // N is guaranteed odd here, so (N-1)/2 is an exact integer.
    const int centre = (N - 1) / 2;
    const double centre_d = static_cast<double>(centre);
    const double inv_i0_beta = 1.0 / bessel_i0(beta);

    double sum = 0.0;
    for (int n = 0; n < N; ++n) {
        const double m = static_cast<double>(n - centre);

        // Ideal sinc (scaled so 2*fc is the normalised cutoff in
        // cycles/sample). h_ideal[n] = 2*fc * sinc(2*fc * m).
        const double ideal = 2.0 * fc * sinc_pi(2.0 * fc * m);

        // Kaiser window. arg = 2n/(N-1) - 1 in [-1, 1].
        const double arg = m / centre_d;
        const double inside = 1.0 - arg * arg;
        const double root = inside > 0.0 ? std::sqrt(inside) : 0.0;
        const double w = bessel_i0(beta * root) * inv_i0_beta;

        const double tap = ideal * w;
        out[n] = tap;
        sum += tap;
    }
    return sum;
}

// Round up to the nearest odd integer >= n.
inline int round_up_odd(int n) noexcept {
    if (n < 1) {
        return 1;
    }
    return (n % 2 == 1) ? n : n + 1;
}

// --------------------------------------------------------------
// Rate validation
// --------------------------------------------------------------

bool is_supported_rate(unsigned host_rate) noexcept {
    switch (host_rate) {
        case 44100:
        case 48000:
        case 96000:
        case 192000:
            return true;
        default:
            return false;
    }
}

// --------------------------------------------------------------
// Per-channel dot-product helper
// --------------------------------------------------------------
//
// Reads `N` consecutive samples from a ring buffer of size `N`
// starting at index `start_idx`, multiplies by `coeffs` (also
// length N, in natural order), and returns the dot product.
//
// Precondition: 0 <= start_idx < N. The ring buffer is `buf[0..N-1]`.
// We read (buf[start_idx], buf[start_idx+1], ..., buf[start_idx+N-1])
// with indices taken mod N, and the tap weights in order
// (coeffs[0], coeffs[1], ..., coeffs[N-1]). Accumulator is
// double to keep summation error small on long filters.
inline double dot_ring(const float* buf, const float* coeffs,
                       int N, int start_idx) noexcept {
    double acc = 0.0;
    const int head = N - start_idx;  // count until wrap
    // First segment: buf[start_idx..N-1] paired with coeffs[0..head-1].
    for (int i = 0; i < head; ++i) {
        acc += static_cast<double>(buf[start_idx + i])
             * static_cast<double>(coeffs[i]);
    }
    // Second segment: buf[0..start_idx-1] paired with coeffs[head..N-1].
    for (int i = 0; i < start_idx; ++i) {
        acc += static_cast<double>(buf[i])
             * static_cast<double>(coeffs[head + i]);
    }
    // Anti-denormal bias: forces the float cast in the caller to
    // land on a normal number rather than a denormal that would
    // poison the ring buffer on the next loop-back. Cancels to
    // zero at float precision.
    return acc + kDenormalBias - kDenormalBias;
}

}  // namespace

// --------------------------------------------------------------
// Public API
// --------------------------------------------------------------

bool decimator_rate_supported(unsigned host_rate) noexcept {
    return is_supported_rate(host_rate);
}

bool decimator_init(Decimator* d, unsigned host_rate) noexcept {
    if (d == nullptr) {
        return false;
    }
    // Zero-initialise everything so failure paths and post-init
    // reads are well-defined regardless of how the caller
    // constructed d. Callers who use `Decimator d{};` already
    // pay this cost; callers who use bare `Decimator d;` and
    // a later process()/reset_state() call would otherwise read
    // indeterminate memory through filter_length. Cheap (~17 KB
    // memset, cold path), high defensive value.
    *d = Decimator{};
    if (!is_supported_rate(host_rate)) {
        return false;
    }

    d->host_rate = host_rate;
    d->internal_rate = host_rate * static_cast<unsigned>(kDecimationFactor);

    // Passband / stopband in cycles/sample (normalised to
    // internal rate). Passband edge 0.45 x host = 0.1125 x
    // internal; stopband edge 0.50 x host = 0.1250 x internal.
    // The -6 dB sinc cutoff sits at the arithmetic midpoint of
    // those two edges, which centres the transition band and
    // minimises the overshoot at the stopband corner.
    const double host_over_internal =
        1.0 / static_cast<double>(kDecimationFactor);
    const double passband_edge = 0.45 * host_over_internal;   // 0.1125
    const double stopband_edge = 0.50 * host_over_internal;   // 0.1250
    const double fc = 0.5 * (passband_edge + stopband_edge);  // 0.11875
    const double dw = stopband_edge - passband_edge;          // 0.0125

    // Kaiser parameters.
    const double beta = kaiser_beta(kStopbandAttenuationDb);

    // N estimate: Oppenheim & Schafer eq. 7.75 in cycles-per-
    // sample form is (A - 8) / (2.285 * 2*pi * dw). Round up to
    // odd, clamp to kMaxFilterLength.
    const double two_pi = 2.0 * kPi;
    int N = static_cast<int>(std::ceil(
        (kStopbandAttenuationDb - 8.0) / (2.285 * two_pi * dw)));
    N = round_up_odd(N);
    if (N < 3) {
        N = 3;
    }
    if (N > kMaxFilterLength) {
        N = kMaxFilterLength;
        if ((N % 2) == 0) {
            N -= 1;  // kMaxFilterLength = 513 is odd, this is a guard.
        }
    }

    // Design taps in double precision, then normalise DC gain to
    // 1.0 and store as float in natural order.
    double taps[kMaxFilterLength];
    double sum = design_kaiser_lowpass(N, fc, beta, taps);
    if (sum <= 0.0) {
        // Pathological: should not happen for the supported rate
        // set, but make the failure explicit rather than emit a
        // divide-by-zero filter.
        d->filter_length = 0;
        return false;
    }
    const double inv_sum = 1.0 / sum;
    double normalised_sum = 0.0;
    for (int i = 0; i < N; ++i) {
        const double t = taps[i] * inv_sum;
        d->coeffs[i] = static_cast<float>(t);
        normalised_sum += t;
    }
    // Zero the unused tail so downstream code reading beyond N
    // (there is none, but defensive) sees silence.
    for (int i = N; i < kMaxFilterLength; ++i) {
        d->coeffs[i] = 0.0f;
    }

    d->filter_length = N;
    d->dc_gain = normalised_sum;

    decimator_reset_state(d);
    return true;
}

void decimator_reset_state(Decimator* d) noexcept {
    if (d == nullptr || d->filter_length <= 0) {
        return;
    }
    d->write_pos = 0;
    for (int c = 0; c < kNumChannels; ++c) {
        std::memset(d->buffers[c], 0,
                    sizeof(float) * static_cast<std::size_t>(d->filter_length));
    }
}

int decimator_process(Decimator* d,
                      const float* in_ch1, const float* in_ch2,
                      const float* in_ch3, const float* in_ch4,
                      int in_count,
                      float* out_ch1, float* out_ch2,
                      float* out_ch3, float* out_ch4) noexcept {
    if (d == nullptr || d->filter_length <= 0) {
        return -1;
    }
    if (in_count < 0) {
        return -1;
    }
    if ((in_count % kDecimationFactor) != 0) {
        return -1;
    }
    if (in_count == 0) {
        return 0;
    }

    const int N = d->filter_length;
    int write_pos = d->write_pos;
    int out_written = 0;

    // Pointer arrays indexed by channel to keep the inner loops
    // symmetric. `kNumChannels` is a compile-time constant; this
    // unrolls cleanly under -O2.
    const float* in_p[kNumChannels] = {in_ch1, in_ch2, in_ch3, in_ch4};
    float* out_p[kNumChannels] = {out_ch1, out_ch2, out_ch3, out_ch4};
    float* bufs[kNumChannels] = {
        d->buffers[0], d->buffers[1], d->buffers[2], d->buffers[3]};

    // Phase invariant: every call consumes an integer number of
    // decimation groups (enforced above), so i == 0 always starts
    // a fresh group and the emit aligns cleanly across calls.
    for (int i = 0; i < in_count; ++i) {
        // Push one input sample per channel into its ring
        // buffer at write_pos.
        for (int c = 0; c < kNumChannels; ++c) {
            bufs[c][write_pos] = in_p[c][i];
        }

        // Advance write_pos.
        int next_write = write_pos + 1;
        if (next_write >= N) {
            next_write = 0;
        }

        // On every kDecimationFactor-th sample (i.e. when
        // (i + 1) is a multiple of kDecimationFactor), compute
        // one output sample per channel. The window to read is
        // (buffers oldest-first), which is the slot right after
        // the one we just wrote (i.e. next_write is the oldest
        // after the wrap).
        if (((i + 1) % kDecimationFactor) == 0) {
            for (int c = 0; c < kNumChannels; ++c) {
                const double acc =
                    dot_ring(bufs[c], d->coeffs, N, next_write);
                out_p[c][out_written] = static_cast<float>(acc);
            }
            ++out_written;
        }

        write_pos = next_write;
    }

    d->write_pos = write_pos;
    return out_written;
}

// --------------------------------------------------------------
// HostResampler implementation
// --------------------------------------------------------------
//
// Design: see rackboy_resampler.hpp header block. The kernel is a
// Kaiser-windowed-sinc evaluated per output sample at the exact
// fractional input-time offset (not a classical polyphase table).
// This trades a ~2x multiply cost over a polyphase 2:1/4:1
// interpolator for uniform code across the 4 supported host rates
// and avoids the memory blowup of a 147-phase bank for 44.1 kHz.

namespace {

// gcd for small positive integers. Euclidean.
int int_gcd(int a, int b) noexcept {
    if (a < 0) { a = -a; }
    if (b < 0) { b = -b; }
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

// Ring-buffer mask for fast modulo.
constexpr int kHistoryMask = kHostResamplerHistorySize - 1;

// Returns the oldest input index that must be readable for emit
// of output index `m` to cover its symmetric kernel window. Exact
// integer arithmetic avoids fp drift on long runs.
//
// Output sample m has fractional input-time t = m * M / L (APU
// samples from the start). The kernel reads input indices in
// [ceil(t) + k - half_taps] for k in [0, N). The latest index
// needed is base + half_taps where base = floor(t). Stored
// separately so required_inputs can match push_input's bookkeeping.
long long latest_needed_input_index(const rackboy::resampler::HostResampler* hr,
                                    long long m) noexcept {
    // base = (m * M) / L with exact division. Since M, L are small
    // (<= 160 and <= 192000 respectively) and m fits in int64, the
    // multiply cannot overflow in practice for any realistic run
    // duration; the worst-case product is bounded by ~10^10 * 160
    // = 1.6e12, still well inside int64.
    const long long num = m * static_cast<long long>(hr->M);
    const long long base = num / static_cast<long long>(hr->L);
    return base + hr->half_taps;
}

// Push into the ring without advancing input_count. Used by the
// public push_input after bookkeeping is settled so the history
// slot matches input_count - 1 on exit (i.e. "the most recent
// sample written is at absolute index input_count - 1").
void write_history(rackboy::resampler::HostResampler* hr,
                   long long absolute_idx,
                   float ch1, float ch2, float ch3, float ch4) noexcept {
    const int slot = static_cast<int>(absolute_idx & kHistoryMask);
    hr->history[0][slot] = ch1;
    hr->history[1][slot] = ch2;
    hr->history[2][slot] = ch3;
    hr->history[3][slot] = ch4;
}

// Read ring slot by absolute index. Caller guarantees the slot has
// not been overwritten (enforced by kHostResamplerHistorySize >=
// N_taps and the required_inputs contract).
inline float read_history(const rackboy::resampler::HostResampler* hr,
                          int channel, long long absolute_idx) noexcept {
    const int slot = static_cast<int>(absolute_idx & kHistoryMask);
    return hr->history[channel][slot];
}

}  // namespace

bool host_resampler_rate_supported(unsigned host_rate) noexcept {
    switch (host_rate) {
        case 44100u:
        case 48000u:
        case 96000u:
        case 192000u:
            return true;
        default:
            return false;
    }
}

bool host_resampler_init(HostResampler* hr, unsigned host_rate) noexcept {
    if (hr == nullptr) {
        return false;
    }
    // Zero-init covers the failure path and guarantees any later
    // reset_state / emit on a failed init reads well-defined memory.
    *hr = HostResampler{};
    if (!host_resampler_rate_supported(host_rate)) {
        return false;
    }

    hr->host_rate = host_rate;
    hr->apu_rate = kApuRateHz;

    const int gcd = int_gcd(static_cast<int>(host_rate),
                            static_cast<int>(kApuRateHz));
    hr->L = static_cast<int>(host_rate) / gcd;
    hr->M = static_cast<int>(kApuRateHz) / gcd;

    // `fc` = min(1, fs_out/fs_in) in the [0, 1] convention where
    // fc = 1 means "cut off at fs_in/2". DC gain of
    // fc * sinc(fc * tau) integrated across a symmetric infinite
    // tap window is exactly 1, so no post-design normalisation
    // is needed -- only the windowing contributes a tiny bounded
    // sum error which tests assert is within float epsilon.
    const double fs_in = static_cast<double>(hr->apu_rate);
    const double fs_out = static_cast<double>(hr->host_rate);
    hr->fc = fs_out < fs_in ? fs_out / fs_in : 1.0;

    const int N = kHostResamplerNumTaps;
    hr->half_taps = (N - 1) / 2;

    // Precompute the Kaiser window across [0, N). Same formula as
    // the spike: w[k] = I0(beta * sqrt(1 - (2k/(N-1) - 1)^2)) /
    // I0(beta). Symmetric; precomputing avoids recomputing bessel
    // I0 per-sample on the audio thread.
    const double beta = kHostResamplerKaiserBeta;
    const double inv_i0_beta = 1.0 / bessel_i0(beta);
    const double N_minus_1 = static_cast<double>(N - 1);
    for (int k = 0; k < N; ++k) {
        const double arg = 2.0 * static_cast<double>(k) / N_minus_1 - 1.0;
        const double inside = 1.0 - arg * arg;
        const double root = inside > 0.0 ? std::sqrt(inside) : 0.0;
        const double w = bessel_i0(beta * root) * inv_i0_beta;
        hr->win[k] = static_cast<float>(w);
    }

    host_resampler_reset_state(hr);
    return true;
}

void host_resampler_reset_state(HostResampler* hr) noexcept {
    if (hr == nullptr) {
        return;
    }
    for (int c = 0; c < kNumChannels; ++c) {
        std::memset(hr->history[c], 0, sizeof(hr->history[c]));
    }
    hr->input_count = 0;
    hr->output_count = 0;
}

int host_resampler_required_inputs(const HostResampler* hr) noexcept {
    if (hr == nullptr || hr->L <= 0) {
        return 0;
    }
    const long long latest =
        latest_needed_input_index(hr, hr->output_count);
    // input_count is the number of samples already pushed; absolute
    // indices 0..input_count - 1 are present. Need latest <=
    // input_count - 1, i.e. input_count >= latest + 1.
    const long long need = (latest + 1) - hr->input_count;
    if (need <= 0) {
        return 0;
    }
    // Defensive narrowing clamp. In practice `need` is bounded by
    // `half_taps + 1 = 129` on the first emit and <= 2 in steady
    // state, but a corrupted `input_count` on a partially-initialised
    // struct could drive the cast past INT_MAX. Clamping to the
    // kernel width (the largest legitimate burst) keeps the caller's
    // loop bounded without hiding the anomaly.
    const long long max_need =
        static_cast<long long>(kHostResamplerNumTaps);
    if (need > max_need) {
        return kHostResamplerNumTaps;
    }
    return static_cast<int>(need);
}

void host_resampler_push_input(HostResampler* hr,
                                float in_ch1, float in_ch2,
                                float in_ch3, float in_ch4) noexcept {
    if (hr == nullptr || hr->L <= 0) {
        return;
    }
    const long long idx = hr->input_count;
    write_history(hr, idx, in_ch1, in_ch2, in_ch3, in_ch4);
    hr->input_count = idx + 1;
}

void host_resampler_emit(HostResampler* hr,
                          float* out_ch1, float* out_ch2,
                          float* out_ch3, float* out_ch4) noexcept {
    if (hr == nullptr || hr->L <= 0) {
        if (out_ch1 != nullptr) { *out_ch1 = 0.0f; }
        if (out_ch2 != nullptr) { *out_ch2 = 0.0f; }
        if (out_ch3 != nullptr) { *out_ch3 = 0.0f; }
        if (out_ch4 != nullptr) { *out_ch4 = 0.0f; }
        return;
    }

    const int N = kHostResamplerNumTaps;
    const int half = hr->half_taps;
    const double fc = hr->fc;

    // Fractional input-time of this output sample in exact rational
    // form: t = m * M / L. Compute base (floor(t)) and frac (t - base)
    // in integer arithmetic to avoid fp drift.
    const long long m = hr->output_count;
    const long long num = m * static_cast<long long>(hr->M);
    const long long L_ll = static_cast<long long>(hr->L);
    const long long base_ll = num / L_ll;
    const long long rem = num - base_ll * L_ll;
    const double frac = static_cast<double>(rem) / static_cast<double>(hr->L);

    // Accumulators in double to keep summation error well below the
    // 1 ULP float budget. Bias + cancel idiom handles anti-denormal.
    double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;

    for (int k = 0; k < N; ++k) {
        const long long src_idx = base_ll + (k - half);
        // tau is the time offset from the current output moment to
        // input index src_idx, measured in APU samples. src_idx is
        // the k-th tap's input index; output time is (base + frac),
        // so tau = (base + frac) - src_idx = frac - (k - half).
        const double tau = frac - static_cast<double>(k - half);
        const double s = sinc_pi(fc * tau);
        const double coef = fc * s * static_cast<double>(hr->win[k]);

        // Out-of-range guards: src_idx may be < 0 near startup (when
        // output_count outruns the pre-roll we push) or so old that
        // the ring slot was overwritten. required_inputs gates the
        // latter; the former is only reachable if a caller pre-emits
        // before pushing any input. Treat out-of-range as zero.
        if (src_idx < 0 || src_idx >= hr->input_count) {
            continue;
        }
        acc0 += static_cast<double>(read_history(hr, 0, src_idx)) * coef;
        acc1 += static_cast<double>(read_history(hr, 1, src_idx)) * coef;
        acc2 += static_cast<double>(read_history(hr, 2, src_idx)) * coef;
        acc3 += static_cast<double>(read_history(hr, 3, src_idx)) * coef;
    }

    // Denormal guard: the +bias - bias idiom cancels to zero at
    // float precision but lands the cast on a normal number.
    acc0 = acc0 + kDenormalBias - kDenormalBias;
    acc1 = acc1 + kDenormalBias - kDenormalBias;
    acc2 = acc2 + kDenormalBias - kDenormalBias;
    acc3 = acc3 + kDenormalBias - kDenormalBias;

    if (out_ch1 != nullptr) { *out_ch1 = static_cast<float>(acc0); }
    if (out_ch2 != nullptr) { *out_ch2 = static_cast<float>(acc1); }
    if (out_ch3 != nullptr) { *out_ch3 = static_cast<float>(acc2); }
    if (out_ch4 != nullptr) { *out_ch4 = static_cast<float>(acc3); }

    hr->output_count = m + 1;
}

}  // namespace resampler
}  // namespace rackboy
