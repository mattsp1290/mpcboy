// rackboy_core/rackboy_gate.h
//
// Rising-edge detector with Schmitt-trigger hysteresis for the two
// external gate inputs (Gate 1 = trigger-all, Gate 2 = table clock).
// Bead rackboy-gn4.
//
// The two gates are sampled once per audio-callback frame (48 samples
// at 48 kHz on Daisy, variable but similar on VCV). Each gate input is
// a normalized float — on VCV Rack the module shim converts the 0..10 V
// CV to [0.0, 1.0]; on Daisy the ADC reads into a float of similar
// shape. This module turns that continuous float into a stream of
// discrete "rising edge" booleans, with a Schmitt hysteresis band to
// reject CV noise and slow-slope ambiguity near the nominal threshold.
//
// Spec constraints (from docs/prompts/rackboy-lsdj-quad-voice-module.md):
//   * Gate 1 must handle up to 1 kHz rising-edge rate.
//   * Gate 2 must handle up to 1 kHz rising-edge rate.
//   * At 48 kHz sample rate that is at minimum 48 samples between
//     successive rising edges — orders of magnitude looser than the
//     hysteresis band can produce, so the 1 kHz ceiling is trivially
//     met as long as the detector emits exactly one rising-edge flag
//     per Schmitt low→high transition.
//   * CV noise around the threshold must not produce spurious edges.
//
// Real-time contract: pure POD struct, `noexcept` helpers, no
// allocation, no STL, no throw. Safe to call from the audio thread.

#ifndef RACKBOY_CORE_RACKBOY_GATE_H_
#define RACKBOY_CORE_RACKBOY_GATE_H_

#include <cmath>
#include <type_traits>

namespace rackboy {
namespace core {

// Default Schmitt thresholds expressed in the normalized [0.0, 1.0]
// input space the rest of rackboy uses for gate lines. 0.8 / 0.2 gives
// a wide hysteresis band centered on the middle of the input range,
// which tolerates significant CV noise without losing real edges. The
// specific values are not normative — callers may pass custom
// thresholds per input, e.g. a tighter band on the VCV Rack side
// where CV is numerically clean, or a looser band on Daisy for
// heavier ADC noise.
constexpr float kDefaultGateRisingThreshold  = 0.8f;
constexpr float kDefaultGateFallingThreshold = 0.2f;

// GateState — per-input Schmitt detector state. Pure data POD, owned
// one-per-gate-input by rackboy_core (two per module in v1: Gate 1
// and Gate 2). The `high` member records whether the Schmitt output
// is currently "latched high"; a rising edge fires on the sample that
// flips `high` from false to true.
struct GateState {
    bool high;
};

static_assert(std::is_trivially_copyable<GateState>::value,
              "GateState must stay trivially copyable — no user-provided "
              "ctors, no non-trivial members.");
static_assert(std::is_standard_layout<GateState>::value,
              "GateState must stay standard-layout so host tests and the "
              "Daisy target agree on offsets.");

// Reset a gate state to "power-on low": `high == false`, so the very
// next sample that clears the rising threshold fires a rising edge.
// Called at construction, on explicit reset, and after a bank swap so
// stale pre-swap state cannot ghost-fire an edge. Reset-then-same-
// input-again re-arms the edge, honoring the Gate 1 retrigger contract
// from Architecture Decision #9
// (docs/prompts/rackboy-lsdj-quad-voice-module.md).
inline void gate_reset(GateState& s) noexcept
{
    s.high = false;
}

// Advance the detector by one sample. Returns `true` iff this sample
// produced a rising edge (Schmitt output transitioned from low to
// high).
//
// Non-finite input handling: NaN is treated as 0.0f (detector drops to
// low / never fires), matching the NaN-as-zero convention in
// rackboy_control.cpp. +inf saturates to the high rail (fires if low,
// stays latched if already high); -inf saturates to the low rail.
//
// Threshold-sanity handling: if the caller passes thresholds with
// `rising < falling` the function swaps them locally on the stack so
// the Schmitt hysteresis band stays well-defined. Without this guard,
// a constant input sitting inside an inverted band produces a rising
// edge every other sample. `rising == falling` is allowed and
// degenerates to a plain comparator at that single value.
//
// Defined inline because the body is trivial, the audio thread calls
// this once per sample, and we do not want to pay the out-of-line call
// cost without requiring LTO.
inline bool gate_step(GateState& s,
                      float input,
                      float rising_threshold,
                      float falling_threshold) noexcept
{
    // NaN -> 0.0f.
    if (std::isnan(input)) {
        input = 0.0f;
    }
    // Swap locally if the caller supplied an inverted band. This is
    // the only branch we pay in the nominal case and the CPU's branch
    // predictor will nail it.
    if (rising_threshold < falling_threshold) {
        const float tmp  = rising_threshold;
        rising_threshold = falling_threshold;
        falling_threshold = tmp;
    }

    if (!s.high) {
        if (input >= rising_threshold) {
            s.high = true;
            return true;  // rising edge this sample
        }
        return false;
    }
    if (input <= falling_threshold) {
        s.high = false;
    }
    return false;
}

// Convenience overload with the default rackboy thresholds. Inlined so
// the audio thread does not pay the call overhead.
inline bool gate_step(GateState& s, float input) noexcept
{
    return gate_step(s, input,
                     kDefaultGateRisingThreshold,
                     kDefaultGateFallingThreshold);
}

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_GATE_H_
