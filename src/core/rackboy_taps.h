// rackboy_core/rackboy_taps.h
//
// Per-channel tap routing with DC-cleanup and NaN/denormal guard.
// Routes the four APU per-channel float buffers produced by
// rackboy_apu_run_samples() to output pins with signal conditioning.
//
// Tracked by bead rackboy-xpe.
//
// Signal path per channel:
//   APU tap buffer (float, ~[-1,1]) → DC-blocking high-pass filter
//   → NaN/denormal flush-to-zero → output buffer (float)
//
// The DC blocker is a first-order IIR high-pass:
//   y[n] = x[n] - x[n-1] + alpha * y[n-1]
// with alpha = 0.995 (~14 Hz cutoff at 48 kHz). This removes the DC
// component the Game Boy APU's band-limited synthesis can introduce
// while preserving all audible frequencies.
//
// No cross-channel bleed: each channel is processed independently.
// No allocations, no STL, no virtuals. All functions are noexcept.
// Safe to call from the audio thread.

#ifndef RACKBOY_CORE_RACKBOY_TAPS_H_
#define RACKBOY_CORE_RACKBOY_TAPS_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rackboy_voice.h"  // for kNumVoices

namespace rackboy {
namespace core {

// DC-blocker coefficient. Determines the cutoff frequency of the
// first-order high-pass filter:
//   f_c ≈ (1 - alpha) * sample_rate / (2 * pi)
// At alpha = 0.995, f_c ≈ 14 Hz at 48 kHz — well below audible
// range. The exact value is not load-bearing; anything in [0.99, 0.999]
// is fine for removing Game Boy APU DC offset.
constexpr float kDcBlockerAlpha = 0.995f;

// Threshold below which a float is flushed to zero. Catches denormals
// and near-zero values that would waste CPU in downstream processing.
// 1e-20 is well below the noise floor of any DAC or audio interface.
constexpr float kDenormalThreshold = 1e-20f;

// Per-channel DC blocker state. One per voice (4 total).
struct TapChannelState {
    float prev_in;   // x[n-1]
    float prev_out;  // y[n-1]
};

// Aggregate state for all four channels.
struct TapState {
    TapChannelState ch[kNumVoices];
};

static_assert(std::is_trivially_copyable<TapState>::value,
              "TapState must stay trivially copyable for audio-thread use.");
static_assert(std::is_standard_layout<TapState>::value,
              "TapState must stay standard-layout.");

// Reset all DC blocker state to zero. Called at construction, on bank
// swap-in, or whenever the signal path needs a clean restart (avoids
// a pop from stale filter state).
inline void taps_reset(TapState& s) noexcept
{
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        s.ch[i].prev_in  = 0.0f;
        s.ch[i].prev_out = 0.0f;
    }
}

// Process `n` samples for a single channel through the DC blocker and
// NaN/denormal guard. Reads from `in`, writes to `out`. `in` and `out`
// may alias (in-place processing is supported). If `in` is null, `out`
// is filled with zeros and the filter state is flushed.
void taps_process_channel(TapChannelState& state,
                          std::uint32_t n,
                          const float* in,
                          float* out) noexcept;

// Process all four channels. Convenience wrapper that calls
// taps_process_channel for each voice. Null input buffers produce
// zeroed output. Null output pointers skip that channel entirely
// (filter state is preserved, not flushed). Call taps_reset() if
// you need to flush state for a skipped channel.
void taps_process(TapState& state,
                  std::uint32_t n,
                  const float* ch1_in, const float* ch2_in,
                  const float* ch3_in, const float* ch4_in,
                  float* ch1_out, float* ch2_out,
                  float* ch3_out, float* ch4_out) noexcept;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_TAPS_H_
