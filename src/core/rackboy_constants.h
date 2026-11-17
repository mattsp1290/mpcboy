// rackboy_core/rackboy_constants.h
//
// Lowest-level rackboy_core constants shared across multiple modules in
// the shared DSP tree. Anything in this header must:
//
//   * be a plain `constexpr` value (no runtime state, no function
//     definitions beyond constexpr);
//   * be truly cross-module — if a constant only makes sense inside a
//     single .h/.cpp pair, leave it there;
//   * have a short comment explaining *why* it is shared, so a future
//     reviewer doesn't delete it without understanding the coupling.
//
// Keep this header narrow. Growing it into a grab-bag of unrelated
// numeric literals defeats its purpose.

#ifndef RACKBOY_CORE_RACKBOY_CONSTANTS_H_
#define RACKBOY_CORE_RACKBOY_CONSTANTS_H_

#include <cstdint>

namespace rackboy {
namespace core {

// Sentinel value for "this channel has no instrument selected".
//
// Used by both rackboy_voice.h (VoiceState::active_instrument_index)
// and rackboy_control.h (control_to_index return value when the pool
// is empty). The two modules must agree on the exact int16_t value so
// a control mapping can round-trip through a VoiceState field without
// any translation. Defined here so neither header has to include the
// other just to get the sentinel — that avoids an unnecessary
// rackboy_control -> rackboy_voice coupling.
//
// -1 is chosen so the sentinel compares cleanly against any valid
// instrument index (which is non-negative) and against the signed
// arithmetic in control_to_index without a special-case branch.
constexpr std::int16_t kNoInstrument = -1;

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_CONSTANTS_H_
