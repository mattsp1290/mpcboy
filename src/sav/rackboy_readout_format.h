// rackboy_sav/rackboy_readout_format.h
//
// Pure helpers for formatting a per-channel "what is selected right
// now" display string. The VCV widget (src/RackboyWidget.cpp) feeds
// these into its `nvgText` call every draw frame; the Daisy OLED
// readout (bead rackboy-1i2, not yet landed) is expected to call
// into exactly the same helpers so the two front ends stay
// byte-identical for any given (channel, slot, name) tuple.
//
// Why live under rackboy_sav
// --------------------------
// The channel prefix strings ("PU1", "PU2", "WV", "NO") are already
// pinned by the sidecar-JSON key format defined in
// rackboy_sav/rackboy_sav_sidecar.h. Keeping the display format in
// the same directory that owns the key schema means the two cannot
// drift — sidecar keys and widget readouts share a single source of
// truth for the per-channel label. The helper does not depend on
// any other sav internals; it only needs `RackboyInstrumentName`
// for the optional name argument, which callers resolve through
// `Rackboy::lookup_name` or the sidecar struct directly.
//
// Pureness contract
// -----------------
//   * header-only, no allocation, no STL containers, no exceptions
//   * deterministic: the same (channel, idx, name) always produces
//     the same byte sequence
//   * bounded writes: `buf[n-1] == '\0'` is guaranteed whenever
//     `n > 0`; nothing is written when `n == 0`
//   * `noexcept`; safe for the VCV UI thread and the (future) Daisy
//     OLED render path

#ifndef RACKBOY_SAV_RACKBOY_READOUT_FORMAT_H_
#define RACKBOY_SAV_RACKBOY_READOUT_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rackboy {
namespace sav {

// Number of channels the readout recognizes. Must match
// `rackboy::core::kNumVoices` — pinned via an identical
// `static_assert` at the consumer site. We avoid including
// `rackboy_constants.h` here so this header stays free of the core
// DSP dependency chain; the consumer that does depend on core is
// the one place that imports both headers and can enforce the
// invariant.
constexpr int kReadoutChannelCount = 4;

// Channel label strings, indexed 0..3 (Pulse1, Pulse2, Wave, Noise).
// These match the sidecar JSON key prefixes (`PU1-NN`, `PU2-NN`,
// `WV-NN`, `NO-NN`) so a user who hand-authors a sidecar can
// cross-reference the displayed row directly against their JSON
// keys. No trailing space: the format helper inserts `": "` itself,
// so the assembled string is `"PU1: 01"` / `"WV: 01"` / etc.
// without the extra space that a padded prefix would have
// introduced.
constexpr const char* kReadoutChannelPrefix[kReadoutChannelCount] = {
    "PU1", "PU2", "WV", "NO",
};

// Return the channel prefix string for `channel`, or `nullptr` when
// the argument is out of range. Callers treat a null return as
// "unknown channel" and typically render `"??: --"`.
inline const char* readout_channel_prefix(int channel) noexcept
{
    if (channel < 0 || channel >= kReadoutChannelCount) {
        return nullptr;
    }
    return kReadoutChannelPrefix[channel];
}

// Format the currently-selected instrument readout into `buf`.
//
//   buf, n     — destination. `buf[n-1]` is always `'\0'` on return
//                whenever `n > 0`. If `n == 0`, nothing is written
//                and this function returns immediately.
//   channel    — 0..kReadoutChannelCount-1. Out-of-range values
//                render as `"??: --"` so the caller's UI never
//                displays untrusted indices.
//   idx        — the currently-selected instrument slot, or a
//                negative sentinel (e.g. `rackboy::core::kNoInstrument
//                == -1`) meaning "no instrument selected on this
//                channel". Any negative idx renders as
//                `"<prefix>: --"`.
//   name       — optional human-readable name for the slot. Pass
//                nullptr (or a pointer whose first byte is `'\0'`)
//                to fall back to the `"<prefix>: NN"` two-digit
//                slot-number form. When non-null and non-empty,
//                the helper writes `"<prefix>: <name>"`.
//
// The caller is expected to cap `name`'s length through its source
// buffer (the sidecar parser guarantees
// `kMaxInstrumentNameBytes - 1 == 15` printable bytes plus NUL).
// This helper does no length check on `name`: if the caller passes
// an unterminated buffer, snprintf will walk past it. Honour the
// contract at the call site.
inline void format_readout_text(char*        buf,
                                std::size_t  n,
                                int          channel,
                                std::int16_t idx,
                                const char*  name) noexcept
{
    if (buf == nullptr || n == 0) {
        return;
    }

    const char* prefix = readout_channel_prefix(channel);
    if (prefix == nullptr) {
        // Out-of-range channel — display a neutral placeholder.
        std::snprintf(buf, n, "??: --");
        return;
    }

    if (idx < 0) {
        // Covers both `kNoInstrument == -1` and any other sentinel
        // a future writer might introduce below zero. Collapsing
        // the "no instrument" check to a single sign test keeps the
        // logic readable.
        std::snprintf(buf, n, "%s: --", prefix);
        return;
    }

    if (name != nullptr && name[0] != '\0') {
        std::snprintf(buf, n, "%s: %s", prefix, name);
    } else {
        std::snprintf(buf, n, "%s: %02d", prefix,
                      static_cast<int>(idx));
    }
}

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_READOUT_FORMAT_H_
