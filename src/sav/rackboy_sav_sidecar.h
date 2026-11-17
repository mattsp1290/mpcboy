// rackboy_sav/rackboy_sav_sidecar.h
//
// Optional `<savname>.json` sidecar parser: reads a tiny
// hand-authored JSON file that maps `<CHANNEL>-<SLOT>` keys
// to human-readable instrument names, and fills a
// `RackboyBankNames` struct. Bead rackboy-69j.
//
// Motivation
// ----------
// Per the project prompt's Architecture Decision #8 and
// `docs/USING_EXTRACTOR.md`, LSDJ `.sav` files themselves do
// not carry instrument names — the LSDJ binary format has no
// name field. Rackboy's UI (OLED on Daisy, context menu in
// VCV) wants to display a human-readable name per slot so
// users don't have to remember "Pulse 1 slot 3" by number.
//
// The contract is: if a file named `<savname>.json` exists
// next to the loaded `.sav`, rackboy reads it and displays
// the names from the sidecar; otherwise the UI falls back to
// `<CHANNEL>-<SLOT>` formatted strings like `PU1-03`. The
// extractor CLI does not currently emit this sidecar file
// (see `docs/USING_EXTRACTOR.md` §"No JSON sidecar is emitted
// by the CLI today"); authors produce it by hand for now.
//
// Why hand-roll a JSON parser
// ---------------------------
// Rackboy's audio-thread and parser-thread code budgets
// exclude:
//
//   * STL containers (std::string, std::unordered_map, ...)
//   * Heap allocation (new, malloc, ...)
//   * Exceptions
//
// The standard options (nlohmann/json, rapidjson, picojson)
// all violate at least one of these. A 200-line hand-roll
// that parses our strict subset costs less in both binary
// size and cognitive load than integrating a 10k-line
// third-party JSON library, and the parser's scope is small
// enough that the test suite can cover every branch directly.
//
// Sidecar JSON schema (strict subset)
// -----------------------------------
// A sidecar file is a single JSON object at the top level.
// Every entry is a `"KEY": "VALUE"` pair where:
//
//   * KEY   — channel + slot in the same format the UI uses
//             when no sidecar is present. One of:
//
//               PU1-NN  for Pulse1 slot NN  (0 <= NN <= 63)
//               PU2-NN  for Pulse2 slot NN
//               WV-NN   for Wave   slot NN
//               NO-NN   for Noise  slot NN
//
//             NN is always exactly two decimal digits,
//             zero-padded. "PU1-3" is a parse error.
//
//   * VALUE — the instrument name, 0..
//             `kMaxInstrumentNameBytes - 1` ASCII printable
//             bytes. Longer strings return `NameTooLong`.
//
// Example sidecar file:
//
//     {
//         "PU1-00": "LEAD",
//         "PU1-01": "BASS",
//         "WV-00":  "PAD",
//         "NO-00":  "KICK"
//     }
//
// Deliberate omissions from the JSON subset (all are soft
// errors — the parser rejects them rather than guessing):
//
//   * Backslash escapes (`\"`, `\\`, `\n`, `\uXXXX`, ...).
//     The sidecar is author-controlled; names are ASCII.
//   * Non-printable bytes inside strings (0x00-0x1F, 0x7F).
//   * Non-string values (numbers, booleans, arrays, nested
//     objects, null).
//   * Nested objects inside the top-level object.
//   * Trailing commas.
//   * JSON comments (they are not part of the JSON spec
//     anyway).
//   * Duplicate keys that conflict. A later key silently
//     overwrites an earlier one, matching the "last wins"
//     convention used by most JSON libraries.
//   * UTF-8 BOM at the start of the file.
//
// Input contract
// --------------
//
//   json     — pointer to the first byte of the JSON sidecar
//              buffer. The caller reads the whole file into
//              memory (the file is small — typically a few
//              KB). The buffer does NOT need to be
//              null-terminated; the parser stops at
//              `json_len`. May be null (returns `NullJson`).
//
//   json_len — length in bytes of the buffer.
//
//   out_names — destination. May NOT be null; passing null
//               returns `NullOutNames` without touching any
//               memory. On entry, whatever was in *out_names
//               is discarded (the parser calls
//               `names_reset` before doing anything else).
//               On `Ok` return, entries for slots named in
//               the sidecar are filled and every other slot
//               is left as a zero-length (empty) string.
//               On any error other than `NullOutNames`, the
//               destination is left in the reset state so
//               the caller can fall back to the
//               `<CHANNEL>-<SLOT>` naming without further
//               cleanup.
//
// Output contract
// ---------------
// The parser rewrites the matching slot's `text[]` array in
// place. `text[]` is always null-terminated — the parser
// writes the name bytes verbatim followed by a `\0`, and
// never exceeds the buffer (`NameTooLong` fires first on
// over-length input). Empty slots carry `text[0] == '\0'`
// so a UI consumer can test emptiness with a single byte
// read.
//
// Safety / threading: `noexcept`; performs no allocation;
// reads only from `json`, writes only to `*out_names`. The
// parser is intended to run on the UI / parser thread off
// the audio thread, then be handed to the UI thread via
// whatever SPSC / swap mechanism plumbs display state (that
// pipeline lives in future beads).

#ifndef RACKBOY_SAV_RACKBOY_SAV_SIDECAR_H_
#define RACKBOY_SAV_RACKBOY_SAV_SIDECAR_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rackboy_sav_types.h"  // kMaxInstrumentsPerChannel

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// Name storage
// ---------------------------------------------------------------------------

// Maximum bytes per instrument name slot, including the
// trailing null terminator. Picked to match the Daisy OLED
// render budget: a 128x64 panel with a 6x8 font gives 21
// columns, and rackboy reserves "PU1: " (5 chars) as the row
// prefix, leaving 16 bytes for the name. Matching the
// fallback `<CHANNEL>-<SLOT>` format that fits in 6 bytes
// comfortably, 16 is generous but stays under the OLED row
// budget without runtime truncation.
constexpr std::size_t kMaxInstrumentNameBytes = 16;

// One instrument name slot. The `text` field is always
// null-terminated — a reader can treat `text` as a C
// string directly. Empty slots carry `text[0] == '\0'`.
struct RackboyInstrumentName {
    char text[kMaxInstrumentNameBytes];
};

// Per-channel parallel to `RackboyBank`'s instrument pools.
// The sidecar parser and the `RackboyBank` parsers share
// `kMaxInstrumentsPerChannel` so the slot indices match
// one-to-one: if instrument slot 3 on pulse1 is populated
// in the bank, its human-readable name lives at
// `pulse1[3].text` after this parser runs on the matching
// sidecar file.
//
// Deliberate scope note: this struct is NOT merged into
// `RackboyBank` itself. RackboyBank's POD layout is read by
// the audio thread via the SPSC pointer swap in
// rackboy-2i8, and the audio thread has no use for names.
// Adding ~4 KiB of name bytes to every bank swap would
// inflate the audio-thread budget for data only the UI
// thread reads. A future bead can wire `RackboyBankNames`
// into the UI display path separately; this module keeps
// the names alongside but architecturally decoupled.
struct RackboyBankNames {
    RackboyInstrumentName pulse1[kMaxInstrumentsPerChannel];
    RackboyInstrumentName pulse2[kMaxInstrumentsPerChannel];
    RackboyInstrumentName wave  [kMaxInstrumentsPerChannel];
    RackboyInstrumentName noise [kMaxInstrumentsPerChannel];
};

// POD contract: `RackboyBankNames` must stay
// trivially-copyable and standard-layout so a future bead
// can hand it across an SPSC boundary without surprise. The
// `char[]` arrays are POD, so this should succeed as long
// as no future edit adds a constructor or virtual method.
static_assert(std::is_trivially_copyable<RackboyInstrumentName>::value,
              "RackboyInstrumentName must stay trivially "
              "copyable.");
static_assert(std::is_standard_layout<RackboyInstrumentName>::value,
              "RackboyInstrumentName must stay "
              "standard-layout.");
static_assert(sizeof(RackboyInstrumentName) == kMaxInstrumentNameBytes,
              "RackboyInstrumentName must be exactly "
              "kMaxInstrumentNameBytes wide so a future "
              "SPSC memcpy of RackboyBankNames lands in "
              "the expected layout.");

static_assert(std::is_trivially_copyable<RackboyBankNames>::value,
              "RackboyBankNames must stay trivially "
              "copyable so a future SPSC handoff can memcpy "
              "it as a whole.");
static_assert(std::is_standard_layout<RackboyBankNames>::value,
              "RackboyBankNames must stay standard-layout.");

// Zero every byte of every name slot. Empty slots carry
// `text[0] == '\0'`, which a UI consumer can use to detect
// the "no sidecar / use fallback" condition per slot.
inline void names_reset(RackboyBankNames& n) noexcept
{
    // The four per-channel loops are written out longhand
    // rather than loop-over-channels so the audit trail for
    // "which channels the names struct covers" stays
    // readable at a glance without chasing a helper.
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t b = 0; b < kMaxInstrumentNameBytes; ++b) {
            n.pulse1[i].text[b] = '\0';
            n.pulse2[i].text[b] = '\0';
            n.wave  [i].text[b] = '\0';
            n.noise [i].text[b] = '\0';
        }
    }
}

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// Soft-fail return code for `parse_sidecar`. Enumerator
// values are stable across rackboy v1 for log-replay
// tooling (enforced by the `static_assert` block below).
// Matches the HeaderError / InstrumentsError / TablesError /
// WavesError convention: a log-replay tool may print a raw
// int and deserialize it safely years later.
enum class SidecarError : int {
    Ok              = 0,

    // `json == nullptr`.
    NullJson        = 1,

    // `out_names == nullptr`.
    NullOutNames    = 2,

    // The top-level structure isn't a single JSON object
    // with string:string pairs, or the parser hit an
    // unexpected byte. Covers: missing `{`, missing `}`,
    // missing `:`, missing `,`, non-string key, non-string
    // value, control bytes inside a string, backslash
    // escapes (not supported in the strict subset), EOF
    // mid-token, garbage after the closing `}`.
    InvalidJson     = 3,

    // A key parsed cleanly as a string, but its content
    // didn't match the `PU1-NN` / `PU2-NN` / `WV-NN` /
    // `NO-NN` channel prefix format. Includes lower-case
    // channel prefixes, missing / wrong-length slot number,
    // and unknown channel names.
    UnknownChannel  = 4,

    // A key parsed as `<CHAN>-NN` but the slot number NN
    // is out of range (0..kMaxInstrumentsPerChannel - 1).
    SlotOutOfRange  = 5,

    // A value string was longer than
    // `kMaxInstrumentNameBytes - 1` printable bytes (the
    // last byte is always reserved for the null
    // terminator).
    NameTooLong     = 6,
};

static_assert(static_cast<int>(SidecarError::Ok)             == 0,
              "SidecarError::Ok must stay at 0.");
static_assert(static_cast<int>(SidecarError::NullJson)       == 1,
              "SidecarError::NullJson must stay at 1.");
static_assert(static_cast<int>(SidecarError::NullOutNames)   == 2,
              "SidecarError::NullOutNames must stay at 2.");
static_assert(static_cast<int>(SidecarError::InvalidJson)    == 3,
              "SidecarError::InvalidJson must stay at 3.");
static_assert(static_cast<int>(SidecarError::UnknownChannel) == 4,
              "SidecarError::UnknownChannel must stay at 4.");
static_assert(static_cast<int>(SidecarError::SlotOutOfRange) == 5,
              "SidecarError::SlotOutOfRange must stay at 5.");
static_assert(static_cast<int>(SidecarError::NameTooLong)    == 6,
              "SidecarError::NameTooLong must stay at 6.");

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse a sidecar JSON buffer into a RackboyBankNames.
//
// Parameters:
//   json      — pointer to the first byte of the sidecar
//               JSON buffer. The caller reads the file into
//               memory (tiny files, a few KiB at most). The
//               buffer does NOT need to be null-terminated.
//               May be null (returns `NullJson`).
//   json_len  — length in bytes of the buffer.
//   out_names — destination. May NOT be null; passing null
//               returns `NullOutNames` without touching any
//               memory. The parser calls `names_reset`
//               before parsing, so any stale content is
//               discarded. On a non-`NullOutNames` error
//               return the destination is left in the reset
//               state (every slot empty).
//
// Returns: `SidecarError::Ok` on success, one of the other
// enumerators on failure.
//
// Safety: `noexcept`; no allocation; no STL. Bounded by
// `json_len` on every read.
SidecarError parse_sidecar(const char*       json,
                           std::size_t       json_len,
                           RackboyBankNames* out_names) noexcept;

// Return a short human-readable string for a SidecarError.
// Static-lifetime storage; callers do NOT free the returned
// pointer.
const char* sidecar_error_string(SidecarError e) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_SIDECAR_H_
