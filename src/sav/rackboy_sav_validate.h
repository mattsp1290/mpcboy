// rackboy_sav/rackboy_sav_validate.h
//
// Top-level rackboy .sav load façade: composes the five
// lower-level parsers (header, instruments, tables, waves,
// optional sidecar) into a single entry point and reports
// soft-fail errors with a stable phase-tagged result code.
// Bead rackboy-3c5.
//
// Role in the parser pipeline
// ---------------------------
// Rackboy reads an LSDJ `.sav` as follows:
//
//   1. Read the 131072-byte raw file into a buffer.
//   2. Validate the container header (size, magic, active
//      slot, LSDJ 9.4.0 version byte).
//   3. Walk the block allocation table, extract slot 0's
//      block chain, and decompress it into the 32 KiB
//      "decompressed song" blob.
//   4. Parse the instrument block, tables block, and wave
//      frame pool out of that blob into a `RackboyBank`.
//   5. Optionally read `<savname>.json` next to the file
//      and parse it into a `RackboyBankNames`.
//
// Steps 2, 4, and 5 are implemented by the existing
// rackboy_sav modules (rackboy_sav_header, _instruments,
// _tables, _waves, _sidecar). Step 3 — block-chain
// decompression — lives in the sibling
// `rackboy_sav_compression` module (promoted from the
// SPIKE-4 spike under bead rackboy-cbo). This façade takes
// the raw sav AND the decompressed song as two independent
// buffers: the caller is responsible for running the
// decompressor before handing both buffers to
// `load_rackboy_sav`. The split keeps the façade pure
// composition — a follow-up bead can wrap
// `load_rackboy_sav` in a "one-shot from raw-sav" helper
// without changing this module's contract.
//
// What the bead description names
// -------------------------------
// rackboy-3c5 calls out these specific failure modes:
//
//   * not-128KB      -> LoadPhase::HeaderValidation,
//                       sub = HeaderError::SizeMismatch.
//   * unknown version -> LoadPhase::HeaderValidation,
//                       sub = HeaderError::VersionMismatch.
//   * empty slot      -> LoadPhase::HeaderValidation,
//                       sub = HeaderError::ActiveSlotNotZero
//                             or ActiveSlotOutOfRange.
//   * "bad checksum"  -> the LSDJ .sav format has no
//                       checksum field. The bead names this
//                       in passing as a conceptual failure
//                       mode; rackboy does not actually
//                       compute one, so this façade has no
//                       error code for it. If a later LSDJ
//                       revision adds one, a new enumerator
//                       goes at the tail of `LoadPhase`
//                       without renumbering existing values.
//   * "no instruments" -> NOT an error. An empty instrument
//                       pool is a legitimate LSDJ state
//                       (freshly initialized song, every
//                       alloc-map byte at 0). The caller
//                       can detect it after `Ok` with
//                       `bank.pulse1_count + bank.pulse2_count
//                        + bank.wave_count + bank.noise_count
//                        == 0`. This module deliberately
//                       accepts empty songs: a rackboy
//                       module loaded with one should still
//                       output silence correctly rather
//                       than fault the file.
//
// Result type
// -----------
// `LoadResult` is a tiny POD struct carrying two fields:
//
//   phase           — which sub-parser stage fired the
//                     error, or `LoadPhase::Ok` if the
//                     whole pipeline succeeded.
//   sub_error_code  — the raw int value of the sub-parser
//                     error enum (0 when phase is Ok).
//
// Keeping the sub-error as an `int` means `LoadResult`
// does not force every consumer to `#include` all five
// per-module error headers. Callers that only want a
// human-readable message can feed `LoadResult` into
// `load_error_string` and forget the numeric split
// entirely. Callers that want to distinguish "wrong
// version" from "wrong magic" can branch on `phase` and
// then cast `sub_error_code` back to the matching enum.
//
// Input contract
// --------------
// `LoadInputs` is a plain POD aggregate holding the three
// buffers the load pipeline needs. Every buffer has a
// companion length; nothing is null-terminated. The raw
// .sav and decompressed song are mandatory — passing
// `nullptr` for either triggers a `NullRawSav` or
// `NullDecompressedSong` error under phase
// `ArgumentValidation`. The sidecar is optional: passing
// `sidecar_json == nullptr` with `sidecar_json_len == 0`
// is a "no sidecar present" signal and skips the sidecar
// parse entirely (the function still returns `Ok` when
// everything else passes). Any other combination — a
// null pointer with non-zero length OR a non-null
// pointer with zero length — is a contract violation
// and produces `InconsistentSidecarArgs`. Rationale:
// either mismatch almost always means the caller forgot
// to zero one field when wiring the "optional" path, and
// guessing which way to resolve the inconsistency would
// hide the bug rather than surface it.
//
// Output contract
// ---------------
// On `Ok`, `*out_bank` is fully populated (instruments,
// tables, waves) and `loaded == true`; `*out_names` is
// filled from the sidecar if one was provided, or left
// in a freshly-reset state if no sidecar was provided.
// On any non-Ok return (other than `NullOutBank` /
// `NullOutNames`, which short-circuit before touching
// memory), both `*out_bank` and `*out_names` are left in
// a freshly-reset state so the audio thread can consume
// them as "silent bank" without further cleanup.

#ifndef RACKBOY_SAV_RACKBOY_SAV_VALIDATE_H_
#define RACKBOY_SAV_RACKBOY_SAV_VALIDATE_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rackboy_sav_sidecar.h"  // RackboyBankNames
#include "rackboy_sav_types.h"    // RackboyBank

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// LoadPhase — which stage of the pipeline fired an error
// ---------------------------------------------------------------------------
//
// Enumerator values are stable across rackboy v1 for
// log-replay tooling (enforced by the `static_assert`
// block below). Matches the HeaderError / InstrumentsError
// / TablesError / WavesError / SidecarError convention:
// a log-replay tool may print a raw int and deserialize
// it years later with confidence.
//
// ArgumentValidation is the "before any sub-parser ran"
// phase. It covers null pointer guards on the façade's
// own inputs. HeaderValidation onward covers the
// sub-parser phases in the order they run.
enum class LoadPhase : int {
    Ok                   = 0,

    // Before any sub-parser: null pointer on a required
    // argument or mismatched sidecar pointer/length pair.
    ArgumentValidation   = 1,

    // `validate_header` on the raw .sav failed.
    HeaderValidation     = 2,

    // `parse_instruments` on the decompressed song failed.
    InstrumentBlockParse = 3,

    // `parse_tables` on the decompressed song failed.
    TableBlockParse      = 4,

    // `parse_waves` on the decompressed song failed.
    WaveFramePoolParse   = 5,

    // `parse_sidecar` on the provided sidecar buffer
    // failed. Only reachable when the caller supplied a
    // non-null sidecar pointer.
    SidecarParse         = 6,
};

static_assert(static_cast<int>(LoadPhase::Ok)                   == 0,
              "LoadPhase::Ok must stay at 0.");
static_assert(static_cast<int>(LoadPhase::ArgumentValidation)   == 1,
              "LoadPhase::ArgumentValidation must stay at 1.");
static_assert(static_cast<int>(LoadPhase::HeaderValidation)     == 2,
              "LoadPhase::HeaderValidation must stay at 2.");
static_assert(static_cast<int>(LoadPhase::InstrumentBlockParse) == 3,
              "LoadPhase::InstrumentBlockParse must stay at 3.");
static_assert(static_cast<int>(LoadPhase::TableBlockParse)      == 4,
              "LoadPhase::TableBlockParse must stay at 4.");
static_assert(static_cast<int>(LoadPhase::WaveFramePoolParse)   == 5,
              "LoadPhase::WaveFramePoolParse must stay at 5.");
static_assert(static_cast<int>(LoadPhase::SidecarParse)         == 6,
              "LoadPhase::SidecarParse must stay at 6.");

// ---------------------------------------------------------------------------
// ArgumentError — sub-error codes for the ArgumentValidation phase
// ---------------------------------------------------------------------------
//
// When `LoadPhase::ArgumentValidation` fires, the
// `sub_error_code` field of `LoadResult` holds one of
// these integer values. Unlike HeaderError et al, this
// error space is owned by rackboy_sav_validate itself —
// the sub-parsers have nothing to do with the caller's
// own argument contract.
enum class ArgumentError : int {
    // `out_bank == nullptr`.
    NullOutBank            = 1,

    // `out_names == nullptr`.
    NullOutNames           = 2,

    // `in.raw_sav == nullptr`. Required.
    NullRawSav             = 3,

    // `in.decompressed_song == nullptr`. Required.
    NullDecompressedSong   = 4,

    // `in.sidecar_json == nullptr` but
    // `in.sidecar_json_len != 0`, or vice versa. The
    // absent-sidecar signal is BOTH null AND zero length;
    // any other combination is a contract violation.
    InconsistentSidecarArgs = 5,
};

static_assert(static_cast<int>(ArgumentError::NullOutBank)             == 1,
              "ArgumentError::NullOutBank must stay at 1.");
static_assert(static_cast<int>(ArgumentError::NullOutNames)            == 2,
              "ArgumentError::NullOutNames must stay at 2.");
static_assert(static_cast<int>(ArgumentError::NullRawSav)              == 3,
              "ArgumentError::NullRawSav must stay at 3.");
static_assert(static_cast<int>(ArgumentError::NullDecompressedSong)    == 4,
              "ArgumentError::NullDecompressedSong must stay at 4.");
static_assert(static_cast<int>(ArgumentError::InconsistentSidecarArgs) == 5,
              "ArgumentError::InconsistentSidecarArgs must stay at 5.");

// ---------------------------------------------------------------------------
// LoadResult — phase-tagged return value
// ---------------------------------------------------------------------------
//
// `phase == LoadPhase::Ok` (with `sub_error_code == 0`)
// is the "everything passed" signal. Any other phase has
// `sub_error_code` set to the raw int cast of the
// sub-parser's error enum.
struct LoadResult {
    LoadPhase phase;
    int       sub_error_code;
};

// POD contract: LoadResult must stay trivially copyable
// and standard-layout. A caller may fling this through a
// log macro, an SPSC ring, or a VCV UI toast without
// worrying about ctors / dtors / padding. Adding a
// non-trivial member (e.g. a std::string) would fail the
// build here, long before it caused a mysterious
// memory-layout bug downstream.
static_assert(std::is_trivially_copyable<LoadResult>::value,
              "LoadResult must stay trivially copyable.");
static_assert(std::is_standard_layout<LoadResult>::value,
              "LoadResult must stay standard-layout.");

// Convenience constructors.
inline LoadResult load_ok() noexcept
{
    return LoadResult{LoadPhase::Ok, 0};
}

inline LoadResult load_err(LoadPhase p, int sub) noexcept
{
    return LoadResult{p, sub};
}

// ---------------------------------------------------------------------------
// LoadInputs — POD aggregate of the three input buffers
// ---------------------------------------------------------------------------

struct LoadInputs {
    // Raw LSDJ .sav file contents. Exactly 131072 bytes
    // on any valid input; the header-validator rejects
    // any other size.
    const std::uint8_t* raw_sav;
    std::size_t         raw_sav_len;

    // Decompressed song blob for the active slot. The
    // caller is responsible for running the decompressor
    // (`rackboy::sav::compression::decompress` in
    // `rackboy_sav_compression.h`) before filling this
    // field.
    const std::uint8_t* decompressed_song;
    std::size_t         decompressed_song_len;

    // Optional `<savname>.json` sidecar contents. Pass
    // `{nullptr, 0}` to signal "no sidecar present" —
    // the façade will skip the sidecar parse and return
    // `Ok` on success. Any other null/length combination
    // is an `InconsistentSidecarArgs` error.
    const char*         sidecar_json;
    std::size_t         sidecar_json_len;
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Run the full rackboy load pipeline on the provided
// buffers. On `Ok`, `*out_bank` is fully populated and
// `loaded == true`; `*out_names` is filled from the
// sidecar if one was provided. On any non-Ok return
// (other than the two output-pointer null guards), both
// destinations are left in a freshly-reset state.
//
// Noexcept, no allocation, no exceptions. Intended to be
// called from the UI thread; cheap enough (a fixed walk
// over a 128 KiB file plus a ~32 KiB decompressed blob)
// to run on every `.sav` click.
LoadResult load_rackboy_sav(const LoadInputs& in,
                            RackboyBank*      out_bank,
                            RackboyBankNames* out_names) noexcept;

// Return a short human-readable string for a LoadResult.
// The returned pointer addresses a static literal; the
// caller does NOT free it and the pointer stays valid for
// the life of the program.
//
// The string encodes both the phase and the sub-error
// code, e.g. "header validation: version mismatch" or
// "instrument block parse: channel pool full". Callers
// that only want the phase can branch on
// `result.phase` themselves — this helper is for logging
// and UI toasts where a compact one-line description is
// preferable to a switch-and-concatenate.
const char* load_error_string(LoadResult result) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_VALIDATE_H_
