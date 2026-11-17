// src/rackboy_resampler.hpp
//
// Production resampler from APU internal rate to host rate
// (bead rackboy-45x). Replaces the raw SameBoy cycle-accumulator
// path proven FAIL in SPIKE-3: inter-rate peak spread 1.30 dB vs
// the binding 0.25 dB, non-harmonic spur floor -35..-43 dBFS vs
// the binding -60 dBFS. See docs/spikes/vcv-resampler.md.
//
// Shape
//   * 4:1 integer FIR decimator. (No polyphase subfilter split;
//     the full-rate dot product is computed once per emitted
//     output sample. "Polyphase" optimisation is left for the
//     integration bead rackboy-s20 if profiling warrants it.)
//   * Internal rate = 4 x host rate. The caller drives the APU
//     at the internal rate and feeds those samples through the
//     decimator; every 4 input samples produce one host-rate
//     output sample.
//   * The filter is a Kaiser-windowed low-pass FIR with a
//     transition band at 0.45..0.5 x host rate and stopband
//     attenuation >= 80 dB. Coefficients are computed once in
//     decimator_init(); no allocations or trigonometry after
//     init.
//   * Per-channel FIFO state (ring buffer) for all 4 Game Boy
//     channels. Channels advance in lockstep, so the write
//     pointer is shared.
//
// Thread model
//   * decimator_init: UI / construction thread.
//   * decimator_process: audio thread. No locks, no
//     allocations, no floating-point exceptions beyond what the
//     APU already emits.
//   * Denormals: decimator_process() includes an anti-denormal
//     bias in its accumulator so the cast-back-to-float never
//     produces a denormal (which would cost 10-100x on every
//     downstream multiply). Callers that want belt-and-suspenders
//     protection should still set FTZ/DAZ at audio-callback entry
//     (MXCSR on x86-64, FPCR on ARM). rackboy-s20 (the VCV
//     integration bead) owns that guard in src/Rackboy.hpp; the
//     bias here keeps this module denormal-safe standalone.
//
// Determinism
//   * Filter design uses only double-precision arithmetic with a
//     fixed Taylor-series Bessel I0 evaluation. std::sin and
//     std::sqrt are the only libm transcendentals; across
//     platforms these may differ by a few ULPs of double, which
//     after the final cast to float typically collapses to
//     byte-identical coefficients in practice but is not
//     guaranteed by the standard. A libm-identity record in any
//     determinism-sensitive consumer (e.g. rackboy-5wl's
//     metadata.json) is the belt-and-suspenders fix if exact
//     cross-platform coefficient parity ever matters.
//   * process() is branch-free per tap and accumulates in
//     double to keep summation error below the 1 ULP per-sample
//     threshold expected by tests/audio golden regression
//     (bead rackboy-5wl).
//
// Relationship to the VCV plugin
//   * This header is independent of the VCV Rack SDK and can be
//     tested host-side via tests/resampler/. Wiring the
//     decimator into src/Rackboy.hpp (the audio callback that
//     today calls rackboy_apu_run_samples at host rate) is a
//     separate follow-up bead (rackboy-s20) and is gated on the
//     upstream SPIKE-5 (rackboy-efh). The file reservation for
//     rackboy-45x is strictly src/rackboy_resampler.{hpp,cpp}.
//
// SPIKE-3 acceptance scope
//   * The decimator meets its share of the SPIKE-3 acceptance
//     criteria: -80 dB stopband attenuation, inter-rate
//     amplitude preservation to within float-FFT noise on a
//     clean band-limited input. See
//     tests/resampler/spectral_test.cpp "decimator filter
//     meets its passband and stopband design spec" and
//     "decimator is transparent on a band-limited input" for
//     the binding assertions.
//   * Meeting SPIKE-3 end-to-end (APU -> decimator -> host)
//     requires replacing SameBoy's cycle-accumulator sample
//     emission with a band-limited synthesis path. See
//     docs/DECISIONS.md D-010 for the investigation and
//     rackboy-efh for the follow-up spike.

#ifndef RACKBOY_RESAMPLER_HPP_
#define RACKBOY_RESAMPLER_HPP_

#include <cstddef>
#include <type_traits>

namespace rackboy {
namespace resampler {

// Fixed integer decimation factor. Internal rate = kDecimationFactor
// x host rate. Chosen as the smallest power-of-two factor that lets a
// tractable FIR (a few hundred taps) suppress SameBoy's cycle-
// accumulator aliases below -60 dBFS across all four supported host
// rates. See docs/spikes/vcv-resampler.md "Implications for
// rackboy-45x".
constexpr int kDecimationFactor = 4;

// Maximum filter length. The Kaiser formula for -80 dB attenuation
// and a transition band of 0.0125 x internal rate gives ~401 taps;
// we round up to the next odd number >= 401 and add headroom. The
// filter is always odd-length so the Type-I linear-phase centre tap
// sits at index (N-1)/2.
constexpr int kMaxFilterLength = 513;

// Number of Game Boy channels the decimator tracks (Pulse1, Pulse2,
// Wave, Noise). Matches rackboy::core::kNumVoices.
constexpr int kNumChannels = 4;

// Target stopband attenuation in dB for the windowed-sinc design.
// 80 dB is 20 dB below the -60 dBFS spur criterion so the filter
// does not sit right on the acceptance boundary.
constexpr double kStopbandAttenuationDb = 80.0;

// Decimator state. `decimator_init()` internally zero-assigns the
// whole struct before filling in the filter, so any caller
// construction style (`Decimator d;`, `Decimator d{};`, or
// `memset`) is safe as long as init is called before any other
// public function. Declaring with `Decimator d{};` still costs
// the same as a `memset` and avoids any reliance on init ordering
// for subtle reads of uninitialised members -- it remains the
// recommended style in tests and in production.
struct Decimator {
    // FIR coefficients in natural order: coeffs[0] is the oldest-
    // tap weight, coeffs[filter_length - 1] is the newest-tap
    // weight. Symmetric (Type-I linear phase) by construction.
    float coeffs[kMaxFilterLength];

    // Effective filter length <= kMaxFilterLength. Always odd.
    int filter_length;

    // Per-channel circular history buffer. buffers[c][i] holds
    // the sample written at absolute input index j where
    // (j mod filter_length) == i. On the fast path we read this
    // as a contiguous window by unrolling across the wrap point.
    float buffers[kNumChannels][kMaxFilterLength];

    // Shared write position (all four channels advance in
    // lockstep). Next slot to write is buffers[c][write_pos];
    // after the write, write_pos advances mod filter_length.
    int write_pos;

    // Rates, captured at init for diagnostics and validation.
    unsigned host_rate;
    unsigned internal_rate;

    // DC gain of the designed filter. Kept for tests that want
    // to assert the analytic normalisation held (nominally 1.0).
    double dc_gain;
};

// The decimator must remain trivially copyable / standard-layout
// so callers can embed it in other POD-style structs (e.g. the
// VCV Module state) and so decimator_init can safely use `*d =
// Decimator{};` to zero-fill. These are static assertions, not
// a style check: `std::memset` semantics in the reset path only
// hold if this stays true.
static_assert(std::is_trivially_copyable<Decimator>::value,
              "Decimator must be trivially copyable for memset/assignment");
static_assert(std::is_standard_layout<Decimator>::value,
              "Decimator must be standard layout for offsetof and C ABI");

// Supported host rates for the VCV plugin. Any other value
// aborts init by returning false without touching state beyond
// host_rate / internal_rate / filter_length (set to 0).
bool decimator_rate_supported(unsigned host_rate) noexcept;

// Initialise the decimator for the given host rate. Internal
// rate is set to kDecimationFactor x host_rate. Returns true on
// success; false if host_rate is not one of the supported rates.
// On failure, the caller must not invoke decimator_process().
bool decimator_init(Decimator* d, unsigned host_rate) noexcept;

// Process `in_count` input samples per channel, producing
// `in_count / kDecimationFactor` output samples per channel.
//
// Preconditions:
//   * `in_count` must be a non-negative multiple of
//     kDecimationFactor. An in_count of 0 is a legal no-op.
//   * All input pointers are non-null and hold `in_count`
//     samples.
//   * All output pointers are non-null and hold at least
//     `in_count / kDecimationFactor` writeable samples.
//
// Returns the number of output samples written per channel
// (same for all channels: `in_count / kDecimationFactor`).
// Returns -1 if `in_count` is not a multiple of
// kDecimationFactor (defensive check for integration bugs;
// release builds can skip the test by passing validated inputs).
int decimator_process(Decimator* d,
                      const float* in_ch1, const float* in_ch2,
                      const float* in_ch3, const float* in_ch4,
                      int in_count,
                      float* out_ch1, float* out_ch2,
                      float* out_ch3, float* out_ch4) noexcept;

// Reset the decimator's history buffers to zero without
// recomputing the filter coefficients. Intended for calls from
// Module::onReset / onSampleRateChange where the rate has not
// changed but the caller wants to drop any ring-down energy.
void decimator_reset_state(Decimator* d) noexcept;

// --------------------------------------------------------------
// HostResampler — 48 kHz APU to host-rate streaming resampler
// --------------------------------------------------------------
//
// Production integration primitive for `rackboy-s20`. Implements
// the architecture chosen by `docs/DECISIONS.md` D-012: run the
// SameBoy APU at a pinned 48 kHz (the rate D-011 confirmed is
// natively spur-clean) and fractionally resample the 4-channel
// output to the caller's host rate.
//
// Shape
//   * Kaiser-windowed-sinc time-variant FIR ("Farrow-style"):
//     per output sample, evaluate a 257-tap sinc kernel at the
//     fractional input-time offset, multiplied by a precomputed
//     Kaiser window (beta = 10 → ~100 dB stopband). Equivalent
//     to an infinite-L polyphase bank without the per-rate table
//     size blowup that L = 147 would impose for the 44.1 kHz case.
//   * DC gain normalised to 1.0 by the `fc * sinc(fc * tau)`
//     kernel form (fc = min(1, fs_out/fs_in) per-sample cutoff).
//   * Streaming, per-host-sample API. `required_inputs()` reports
//     how many APU samples must be pushed before the next output
//     is ready; `push_input()` adds one APU sample per channel;
//     `emit()` produces one host-rate sample per channel.
//
// Supported host rates: {44100, 48000, 96000, 192000} — the VCV
// pin set. Any other rate fails init; callers fall back to the
// APU-at-host-rate path and accept the SPIKE-3 spur-floor caveat
// documented in D-011.
//
// Thread model
//   * host_resampler_init / host_resampler_reset_state: UI /
//     construction thread.
//   * host_resampler_required_inputs / push_input / emit: audio
//     thread. No locks, no allocations. `emit` DOES call
//     `std::sin` once per tap (257 x per output sample) on
//     purpose: a precomputed polyphase table for the 44.1 kHz
//     case (L = 147) would cost ~150 KB per module and tie the
//     kernel shape to the supported-rate set; the Farrow-style
//     per-sample evaluation trades a few microseconds of libm
//     for rate-independence. See docs/DECISIONS.md D-012.
//     `push_input` and `required_inputs` are transcendental-free.
//
// Relationship to `Decimator`
//   * `Decimator` remains the 4:1 integer FIR primitive from
//     `rackboy-45x`. It is not wired into the production path by
//     `rackboy-s20`; D-012 retargeted the integration to the
//     48 kHz-pinned architecture. `Decimator` stays in-tree
//     (still exercised by `tests/resampler/`) because its
//     underlying Kaiser-window machinery is the same primitive
//     used here, and a future bead may reuse it for offline
//     analysis where a fixed 4:1 ratio is wanted.
constexpr unsigned kApuRateHz = 48000;

constexpr int kHostResamplerNumTaps = 257;
constexpr double kHostResamplerKaiserBeta = 10.0;

// History ring capacity per channel. Must be >= kHostResamplerNumTaps
// (the widest window read per emit) and, for the modulo-by-mask fast
// path, a power of two. 512 comfortably exceeds 257 without being
// wasteful.
constexpr int kHostResamplerHistorySize = 512;
static_assert((kHostResamplerHistorySize &
               (kHostResamplerHistorySize - 1)) == 0,
              "history size must be a power of two for the mask-based "
              "ring-buffer indexing");
static_assert(kHostResamplerHistorySize >= kHostResamplerNumTaps,
              "history must cover a full tap window");

struct HostResampler {
    // Rates captured at init for diagnostics. apu_rate is always
    // kApuRateHz on success; stored explicitly so tests can assert
    // the pin without digging into global constants.
    unsigned host_rate;
    unsigned apu_rate;

    // Reduced rational L/M where L = host_rate / gcd, M = apu_rate /
    // gcd. Output sample m's input-time is m * M / L (in APU-sample
    // units). Kept as int so the gcd-reduced pair fits comfortably
    // (max L is 192000 before reduction, 40 for 192k/48k after, and
    // 147 for 44.1k/48k after).
    int L;
    int M;

    // Cached kernel design. `fc` = min(1, host/apu); `half_taps` =
    // (N-1)/2, the centre index of the symmetric kernel.
    double fc;
    int half_taps;

    // Precomputed Kaiser window over [0, N). Symmetric, DC-
    // normalised by the `fc * sinc(fc * tau)` kernel form so a
    // DC-1 input streams through to a DC-1 output to within float
    // summation error.
    float win[kHostResamplerNumTaps];

    // Per-channel input history ring. `history[c][i & (size-1)]`
    // holds the APU sample pushed at absolute input index `i`.
    // Capacity >= N_taps + 1 so the symmetric kernel window always
    // fits before the oldest slot is overwritten.
    float history[kNumChannels][kHostResamplerHistorySize];

    // Absolute count of push_input() calls since init. 64-bit so a
    // 48 kHz stream would need ~6 million years to overflow. Used
    // to drive the phase-advance arithmetic as exact integers
    // (m * M) / L, avoiding fp drift over long runs.
    long long input_count;

    // Absolute count of emit() calls. The next output to emit has
    // index output_count; its input-time centre is output_count *
    // M / L.
    long long output_count;
};

static_assert(std::is_trivially_copyable<HostResampler>::value,
              "HostResampler must be trivially copyable for memset/"
              "assignment");
static_assert(std::is_standard_layout<HostResampler>::value,
              "HostResampler must be standard layout");

// Returns true iff `host_rate` is one of the four supported host
// rates {44100, 48000, 96000, 192000}.
bool host_resampler_rate_supported(unsigned host_rate) noexcept;

// Initialise the resampler for the given host rate. On success,
// `hr->apu_rate` is set to kApuRateHz and the Kaiser-window + L/M
// are precomputed; `history` and `input_count`/`output_count` are
// zeroed. Returns false if `host_rate` is unsupported; on failure
// `hr` is left in a zeroed state (safe to call reset_state but
// NOT required_inputs/push_input/emit).
bool host_resampler_init(HostResampler* hr, unsigned host_rate) noexcept;

// Zero the history and reset the phase counters without
// recomputing the Kaiser window or L/M. Callable from
// Module::onReset to drop any ring-down energy without paying the
// init cost.
void host_resampler_reset_state(HostResampler* hr) noexcept;

// Returns the number of `push_input()` calls required before the
// next `emit()` can run safely. Typically 0 (more output than
// input available, e.g. after a 1:4 upsample burst) or 1 (the
// common 1:1 / 2:1 case); may briefly be 2 on a 48 kHz → 44.1 kHz
// phase boundary where the output trails the input by slightly
// more than one APU sample. The returned value is deterministic
// from the resampler's phase state; callers do not need to iterate.
int host_resampler_required_inputs(const HostResampler* hr) noexcept;

// Push exactly one APU sample per channel into the history ring.
// Advances input_count by 1; does not touch output_count. `hr`
// must have been successfully initialised.
void host_resampler_push_input(HostResampler* hr,
                                float in_ch1, float in_ch2,
                                float in_ch3, float in_ch4) noexcept;

// Compute one host-rate output sample per channel. Precondition:
// `host_resampler_required_inputs(hr) == 0` (enough history has
// been pushed to cover the symmetric window around the fractional
// input-time of output sample `output_count`). Emitting with
// insufficient history silently zero-pads the missing taps,
// which is audible as a startup transient; the required_inputs
// contract is the caller-visible guard against that.
//
// Each `out_chN` pointer may be `nullptr`; that channel is then
// skipped (no write). This exists so tests can focus on a single
// channel without allocating dummy outputs; the production
// `Rackboy::process()` path always passes four valid pointers.
void host_resampler_emit(HostResampler* hr,
                          float* out_ch1, float* out_ch2,
                          float* out_ch3, float* out_ch4) noexcept;

}  // namespace resampler
}  // namespace rackboy

#endif  // RACKBOY_RESAMPLER_HPP_
