// rackboy_core/rackboy_taps.cpp
//
// Implementation of per-channel tap routing with DC-cleanup and
// NaN/denormal guard. See rackboy_taps.h for the design commentary.
//
// Tracked by bead rackboy-xpe.

#include "rackboy_taps.h"

#include <cmath>

namespace rackboy {
namespace core {

void taps_process_channel(TapChannelState& state,
                          std::uint32_t n,
                          const float* in,
                          float* out) noexcept
{
    if (!in) {
        // Null input → zero output and flush filter state so the next
        // real buffer doesn't start with a transient from stale history.
        for (std::uint32_t i = 0; i < n; ++i) {
            out[i] = 0.0f;
        }
        state.prev_in  = 0.0f;
        state.prev_out = 0.0f;
        return;
    }

    float xprev = state.prev_in;
    float yprev = state.prev_out;

    for (std::uint32_t i = 0; i < n; ++i) {
        float x = in[i];

        // NaN guard: replace NaN with zero so the filter doesn't
        // propagate garbage downstream.
        if (std::isnan(x)) x = 0.0f;

        // First-order DC-blocking high-pass:
        //   y[n] = x[n] - x[n-1] + alpha * y[n-1]
        float y = x - xprev + kDcBlockerAlpha * yprev;

        // NaN guard on the filter output: if the filter state was
        // corrupted (e.g. uninitialized memory), the computed y may
        // be NaN. Flush to zero so the channel can recover rather
        // than emitting NaN indefinitely.
        if (std::isnan(y)) y = 0.0f;

        // Denormal guard: flush near-zero values to exactly zero.
        // This prevents the CPU from entering the slow denormal
        // arithmetic path on subsequent operations.
        if (std::fabs(y) < kDenormalThreshold) y = 0.0f;

        out[i] = y;
        xprev = x;
        yprev = y;
    }

    state.prev_in  = xprev;
    state.prev_out = yprev;
}

void taps_process(TapState& state,
                  std::uint32_t n,
                  const float* ch1_in, const float* ch2_in,
                  const float* ch3_in, const float* ch4_in,
                  float* ch1_out, float* ch2_out,
                  float* ch3_out, float* ch4_out) noexcept
{
    const float* ins[kNumVoices]  = { ch1_in,  ch2_in,  ch3_in,  ch4_in  };
    float*       outs[kNumVoices] = { ch1_out, ch2_out, ch3_out, ch4_out };

    for (std::size_t ch = 0; ch < kNumVoices; ++ch) {
        if (outs[ch]) {
            taps_process_channel(state.ch[ch], n, ins[ch], outs[ch]);
        }
    }
}

}  // namespace core
}  // namespace rackboy
