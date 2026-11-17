// rackboy_sav/rackboy_sav_validate.cpp
//
// Implementation of the rackboy .sav load façade declared
// in rackboy_sav_validate.h. Bead rackboy-3c5.
//
// The function shape is:
//
//   1. Null-guard the output pointers (out_bank first, so a
//      null names pointer still can't touch the bank).
//   2. Reset both destinations unconditionally.
//   3. Validate the façade-level argument contract
//      (required inputs non-null, sidecar pair
//      consistent).
//   4. Run validate_header on raw_sav.
//   5. Run parse_instruments, parse_tables, parse_waves on
//      the decompressed song, in that order. The iter
//      27-30 precedent documents the order: instruments
//      must run first because it calls bank_reset; tables
//      and waves run afterwards on a bank that already has
//      its instrument pools populated.
//   6. If a sidecar was provided, run parse_sidecar on it.
//   7. Flag the bank as loaded and return Ok.
//
// Any non-Ok return resets both destinations first so the
// audio thread can consume them as "silent bank" without
// further cleanup. This matches the invariant every
// rackboy_sav sub-parser already enforces for its own
// outputs.

#include "rackboy_sav_validate.h"

#include <cstddef>

#include "rackboy_sav_header.h"
#include "rackboy_sav_instruments.h"
#include "rackboy_sav_sidecar.h"
#include "rackboy_sav_tables.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_waves.h"

namespace rackboy {
namespace sav {

namespace {

// Reset both destination structs to a known "silent"
// state. Used at entry AND on every error path so the
// caller never has to look at a half-populated bank.
inline void reset_outputs(RackboyBank&      bank,
                          RackboyBankNames& names) noexcept
{
    bank_reset(bank);
    names_reset(names);
}

// Validate the façade's own argument contract. Produces
// `LoadPhase::ArgumentValidation` on failure.
LoadResult validate_arguments(const LoadInputs& in) noexcept
{
    if (in.raw_sav == nullptr) {
        return load_err(
            LoadPhase::ArgumentValidation,
            static_cast<int>(ArgumentError::NullRawSav));
    }
    if (in.decompressed_song == nullptr) {
        return load_err(
            LoadPhase::ArgumentValidation,
            static_cast<int>(
                ArgumentError::NullDecompressedSong));
    }

    // The absent-sidecar signal is BOTH `nullptr` AND
    // `0 length`. A non-null pointer with zero length or
    // a null pointer with non-zero length is a contract
    // violation — the caller probably forgot to zero out
    // one of the two when setting up an "optional" input.
    const bool sidecar_absent =
        (in.sidecar_json == nullptr) &&
        (in.sidecar_json_len == 0);
    const bool sidecar_present =
        (in.sidecar_json != nullptr) &&
        (in.sidecar_json_len > 0);
    if (!sidecar_absent && !sidecar_present) {
        return load_err(
            LoadPhase::ArgumentValidation,
            static_cast<int>(
                ArgumentError::InconsistentSidecarArgs));
    }

    return load_ok();
}

}  // namespace

LoadResult load_rackboy_sav(const LoadInputs& in,
                            RackboyBank*      out_bank,
                            RackboyBankNames* out_names) noexcept
{
    // (1) Output-pointer null guards. Check out_bank
    //     FIRST so a null names pointer cannot let us
    //     touch the bank, and vice versa. These two short-
    //     circuit before anything is reset — there's
    //     nothing to reset if the destination pointer is
    //     null.
    if (out_bank == nullptr) {
        return load_err(
            LoadPhase::ArgumentValidation,
            static_cast<int>(ArgumentError::NullOutBank));
    }
    if (out_names == nullptr) {
        return load_err(
            LoadPhase::ArgumentValidation,
            static_cast<int>(ArgumentError::NullOutNames));
    }

    // (2) Reset both destinations unconditionally. Every
    //     code path below leaves both in a known state:
    //     freshly reset on any error return, fully
    //     populated on Ok.
    reset_outputs(*out_bank, *out_names);

    // (3) Façade-level argument validation.
    const LoadResult arg_check = validate_arguments(in);
    if (arg_check.phase != LoadPhase::Ok) {
        return arg_check;
    }

    // (4) Header validation on the raw .sav buffer.
    {
        const HeaderError he =
            validate_header(in.raw_sav, in.raw_sav_len);
        if (he != HeaderError::Ok) {
            // Both destinations are already reset from
            // step (2); validate_header does not touch
            // the bank or names.
            return load_err(LoadPhase::HeaderValidation,
                            static_cast<int>(he));
        }
    }

    // (5a) Instrument block. parse_instruments calls
    //      bank_reset itself — we deliberately run it
    //      FIRST so the tables/waves passes see the
    //      bank in a known-empty state before they
    //      populate their own sub-arrays.
    {
        const InstrumentsError ie =
            parse_instruments(in.decompressed_song,
                              in.decompressed_song_len,
                              out_bank);
        if (ie != InstrumentsError::Ok) {
            // parse_instruments already left *out_bank
            // fully reset on failure (bank_reset'd) per
            // its own soft-fail contract, but we still
            // call reset_outputs here on the error path
            // to make the invariant locally legible
            // rather than rely on the sub-parser's
            // contract remaining true forever. It also
            // re-resets *out_names, which parse_instruments
            // does not touch — cheap insurance against any
            // future poison-between-steps scenario.
            reset_outputs(*out_bank, *out_names);
            return load_err(LoadPhase::InstrumentBlockParse,
                            static_cast<int>(ie));
        }
    }

    // (5b) Table block. NOTE: this error branch is
    //      currently unreachable from any end-to-end
    //      test, because the only TablesError values
    //      other than Ok are NullSong / NullOutBank /
    //      SongTooShort — all of which parse_instruments
    //      already short-circuits on in step (5a). The
    //      phase + sub_error_code wiring here is locked
    //      only indirectly by the load_error_string
    //      round-trip test in validate_test.cpp. If a
    //      future TablesError enumerator can fire on a
    //      valid-instrument-block song, add a direct
    //      end-to-end test for this branch.
    {
        const TablesError te =
            parse_tables(in.decompressed_song,
                         in.decompressed_song_len,
                         out_bank);
        if (te != TablesError::Ok) {
            reset_outputs(*out_bank, *out_names);
            return load_err(LoadPhase::TableBlockParse,
                            static_cast<int>(te));
        }
    }

    // (5c) Wave frame pool. NOTE: same coverage gap as
    //      (5b) — the phase + sub_error_code wiring for
    //      this branch is locked only indirectly by the
    //      load_error_string round-trip test. Add a
    //      direct end-to-end test if WavesError gains an
    //      enumerator that can fire on a valid song.
    {
        const WavesError we =
            parse_waves(in.decompressed_song,
                        in.decompressed_song_len,
                        out_bank);
        if (we != WavesError::Ok) {
            reset_outputs(*out_bank, *out_names);
            return load_err(LoadPhase::WaveFramePoolParse,
                            static_cast<int>(we));
        }
    }

    // (6) Optional sidecar. The absent-sidecar signal is
    //     both pointer and length zero (already checked
    //     in validate_arguments above); if either is
    //     set, we parse.
    if (in.sidecar_json != nullptr) {
        const SidecarError se =
            parse_sidecar(in.sidecar_json,
                          in.sidecar_json_len,
                          out_names);
        if (se != SidecarError::Ok) {
            reset_outputs(*out_bank, *out_names);
            return load_err(LoadPhase::SidecarParse,
                            static_cast<int>(se));
        }
    }

    // (7) Mark the bank as loaded. parse_instruments
    //     itself sets `loaded = true` at the end of its
    //     happy path, but we re-assert it here so the
    //     invariant is visible at the façade level: on Ok
    //     return, `out_bank->loaded` is true. This is
    //     idempotent because parse_instruments has
    //     already set it to true if we got this far; any
    //     mismatch would be a sub-parser contract
    //     violation.
    out_bank->loaded = true;
    return load_ok();
}

// ---------------------------------------------------------------------------
// load_error_string
// ---------------------------------------------------------------------------

namespace {

// The five per-phase helpers below each return a
// pre-concatenated static literal combining the phase
// label with the sub-error description. Two different
// `LoadResult` values with the same sub_error_code but
// different phases (e.g. `(TableBlockParse,
// SongTooShort)` vs `(WaveFramePoolParse, SongTooShort)`)
// therefore stringify to distinct messages — honoring the
// "`load_error_string` encodes both the phase AND the
// sub-error code" contract in the header without any
// runtime allocation or snprintf buffer.
//
// Maintenance cost: each helper must grow a new case when
// the matching sub-parser enum grows. The sub-parser
// error_string function's own `-Wswitch-enum` will also
// fire on such a growth, and the façade's no-default-case
// switch here fires alongside it, giving a compile-time
// reminder to update both call sites.

const char* argument_error_string(int sub) noexcept
{
    switch (static_cast<ArgumentError>(sub)) {
        case ArgumentError::NullOutBank:
            return "argument validation: null out_bank";
        case ArgumentError::NullOutNames:
            return "argument validation: null out_names";
        case ArgumentError::NullRawSav:
            return "argument validation: null raw sav buffer";
        case ArgumentError::NullDecompressedSong:
            return "argument validation: null decompressed song buffer";
        case ArgumentError::InconsistentSidecarArgs:
            return "argument validation: sidecar pointer/length mismatch";
    }
    return "argument validation: unknown sub-error";
}

const char* header_validation_error_string(int sub) noexcept
{
    switch (static_cast<HeaderError>(sub)) {
        case HeaderError::Ok:
            return "header validation: ok";
        case HeaderError::NullBuffer:
            return "header validation: null buffer";
        case HeaderError::SizeMismatch:
            return "header validation: size mismatch (not 128 KiB)";
        case HeaderError::MagicMismatch:
            return "header validation: 'jk' magic mismatch";
        case HeaderError::ActiveSlotOutOfRange:
            return "header validation: active slot out of range";
        case HeaderError::ActiveSlotNotZero:
            return "header validation: active slot is not zero (rackboy v1)";
        case HeaderError::VersionMismatch:
            return "header validation: LSDJ version mismatch (expected 9.4.0)";
    }
    return "header validation: unknown sub-error";
}

const char* instrument_block_parse_error_string(int sub) noexcept
{
    switch (static_cast<InstrumentsError>(sub)) {
        case InstrumentsError::Ok:
            return "instrument block parse: ok";
        case InstrumentsError::NullSong:
            return "instrument block parse: null decompressed song buffer";
        case InstrumentsError::NullOutBank:
            return "instrument block parse: null output bank";
        case InstrumentsError::SongTooShort:
            return "instrument block parse: song buffer shorter than 32 KiB";
        case InstrumentsError::AllocMapInvalid:
            return "instrument block parse: alloc map byte not 0x00 or 0x01";
        case InstrumentsError::UnknownType:
            return "instrument block parse: unknown LSDJ instrument type byte";
        case InstrumentsError::ChannelPoolFull:
            return "instrument block parse: per-channel pool full";
    }
    return "instrument block parse: unknown sub-error";
}

const char* table_block_parse_error_string(int sub) noexcept
{
    switch (static_cast<TablesError>(sub)) {
        case TablesError::Ok:
            return "table block parse: ok";
        case TablesError::NullSong:
            return "table block parse: null decompressed song buffer";
        case TablesError::NullOutBank:
            return "table block parse: null output bank";
        case TablesError::SongTooShort:
            return "table block parse: song buffer shorter than 32 KiB";
    }
    return "table block parse: unknown sub-error";
}

const char* wave_frame_pool_parse_error_string(int sub) noexcept
{
    switch (static_cast<WavesError>(sub)) {
        case WavesError::Ok:
            return "wave frame pool parse: ok";
        case WavesError::NullSong:
            return "wave frame pool parse: null decompressed song buffer";
        case WavesError::NullOutBank:
            return "wave frame pool parse: null output bank";
        case WavesError::SongTooShort:
            return "wave frame pool parse: song buffer shorter than 32 KiB";
    }
    return "wave frame pool parse: unknown sub-error";
}

const char* sidecar_parse_error_string(int sub) noexcept
{
    switch (static_cast<SidecarError>(sub)) {
        case SidecarError::Ok:
            return "sidecar parse: ok";
        case SidecarError::NullJson:
            return "sidecar parse: null sidecar buffer";
        case SidecarError::NullOutNames:
            return "sidecar parse: null output names struct";
        case SidecarError::InvalidJson:
            return "sidecar parse: invalid JSON";
        case SidecarError::UnknownChannel:
            return "sidecar parse: unknown channel prefix in key";
        case SidecarError::SlotOutOfRange:
            return "sidecar parse: key slot index out of range";
        case SidecarError::NameTooLong:
            return "sidecar parse: value name too long";
    }
    return "sidecar parse: unknown sub-error";
}

}  // namespace

const char* load_error_string(LoadResult result) noexcept
{
    // No default case on purpose — -Wswitch-enum catches
    // a future LoadPhase addition at compile time,
    // matching the rackboy_sav_{header,instruments,tables,
    // waves,sidecar} convention.
    switch (result.phase) {
        case LoadPhase::Ok:
            return "ok";
        case LoadPhase::ArgumentValidation:
            return argument_error_string(result.sub_error_code);
        case LoadPhase::HeaderValidation:
            return header_validation_error_string(result.sub_error_code);
        case LoadPhase::InstrumentBlockParse:
            return instrument_block_parse_error_string(result.sub_error_code);
        case LoadPhase::TableBlockParse:
            return table_block_parse_error_string(result.sub_error_code);
        case LoadPhase::WaveFramePoolParse:
            return wave_frame_pool_parse_error_string(result.sub_error_code);
        case LoadPhase::SidecarParse:
            return sidecar_parse_error_string(result.sub_error_code);
    }
    // Only reachable if a caller cast an arbitrary int to
    // LoadPhase — e.g. a log-replay tool deserializing a
    // corrupt log line. The enumerator contract in the
    // header pins phases 0..6; anything else is a
    // deserialization bug. We still return a string
    // instead of asserting because logging tools would
    // rather print "unknown" than crash.
    return "unknown LoadPhase value";
}

}  // namespace sav
}  // namespace rackboy
