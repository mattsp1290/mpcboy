// rackboy_sav/rackboy_sav_header.h
//
// Container-layer validation for a LSDJ .sav buffer: size check,
// slot 0 discovery, and LSDJ 9.4.0 header fingerprint verification.
// Bead rackboy-hg5.
//
// What this module does
// ---------------------
// The caller hands us a byte buffer that is *supposed* to be a
// 128 KiB LSDJ .sav file. This module answers a very small set of
// questions:
//
//   1. Is the buffer exactly 131072 bytes? (All LSDJ savs are;
//      anything else is either a different format entirely or a
//      truncated / padded copy, and we do not try to salvage it.)
//
//   2. Does the .sav carry the 'jk' magic at 0x813E?
//      (LSDJ writes these two bytes as a "yes this is an LSDJ sav"
//      fingerprint. Without them, the buffer is almost certainly
//      not an LSDJ sav at all.)
//
//   3. Which slot does the sav advertise as its active project
//      (byte at 0x8140)?  In v1 of rackboy, we only accept slot 0
//      — later beads can relax this if we grow multi-slot UX.
//
//   4. Does the header carry the LSDJ 9.4.0 format-version byte
//      (0x1B at 0x8100)?  LSDJ stores the authoritative version
//      byte in both the decompressed song (at 0x7FFF) and in a
//      header quick-scan copy at 0x8100. The header copy is the
//      one we can check without decompressing anything, so it is
//      what this module looks at.
//
// What this module deliberately does NOT do
// -----------------------------------------
// - It does not decompress slot 0's block stream — that lives in
//   `rackboy_sav_compression.{h,cpp}`. `first_block_for_slot`
//   below does walk the block-allocation table, but only to
//   return the first-block index the decompressor needs as its
//   starting point; all chain-walking stays on the decompressor
//   side.
// - It does not read or decode any instrument bytes.
// - It does not allocate memory, open files, throw, or log.
//
// Every function here is `noexcept`, takes a plain `(const
// uint8_t*, size_t)` input pair, and returns an error code via a
// small POD enum. The caller owns the buffer. Because the module
// is pure data in / POD out, it is safe to call from the UI thread
// as part of a "load .sav" click, from a unit test, or from a
// fuzz harness.
//
// Soft-fail design
// ----------------
// The validation functions report mismatches via a `HeaderError`
// enum rather than aborting, so a caller that gets handed a bogus
// .sav can respond gracefully (error toast in VCV, audio thread
// keeps outputting silence, etc.). Every non-Ok error code maps 1:1
// to a short human-readable string via
// `header_error_string`, which callers can feed into logs or the
// UI without having to maintain their own switch.
//
// File-layout constants
// ---------------------
// The numeric literals below are sourced from
// rackboy_sav/spike/spike_parser.cpp (SPIKE-4 container checks) and
// cross-referenced against the docs/COMPATIBILITY.md binding wire
// contract. They are duplicated here per the mirror policy in
// docs/DECISIONS.md D-007 — see the section banner below for the
// full rationale.

#ifndef RACKBOY_SAV_RACKBOY_SAV_HEADER_H_
#define RACKBOY_SAV_RACKBOY_SAV_HEADER_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// File-layout constants
//
// Mirrored from rackboy_sav/spike/spike_parser.cpp. Per the
// "Mirror policy: spike-vs-production layout constants" section
// of docs/DECISIONS.md D-007, these constants are intentionally
// duplicated: the spike is the SPIKE-4 acceptance oracle and must
// remain independent of the production parser. Spike is canonical;
// any change here must land in the same commit as the matching
// edit in rackboy_sav/spike/spike_parser.cpp.
// ---------------------------------------------------------------------------

// An LSDJ .sav is exactly 128 KiB. Both smaller and larger buffers
// are rejected — LSDJ itself never produces any other size, so a
// mismatch is always either a truncated transfer or a completely
// unrelated file.
constexpr std::size_t kSavSizeBytes = 131072;  // 128 KiB = 0x20000

// Offset of the 'jk' magic bytes LSDJ writes as its container
// fingerprint. Mirrored from spike_parser.cpp::kMagicOffset.
constexpr std::size_t kMagicOffset = 0x813E;
constexpr std::uint8_t kMagicByte0 = 0x6A;  // 'j'
constexpr std::uint8_t kMagicByte1 = 0x6B;  // 'k'

// Offset of the active-project slot byte. A valid LSDJ sav places
// 0..31 here; rackboy v1 only accepts 0x00 (see
// `kRackboyV1ActiveSlot`).
constexpr std::size_t kActiveSlotOffset = 0x8140;

// rackboy v1 only consumes slot 0. A later bead may relax this if
// the UX grows a slot picker.
constexpr std::uint8_t kRackboyV1ActiveSlot = 0x00;

// Slot 0 project name at [0x8000, 0x8008). 8 bytes, NUL-padded.
// Provided as constants so a later bead can surface the name in the
// UI without another offset audit. This module does not *validate*
// the name (any 8 bytes are accepted) — it only exposes the
// offset/length so the caller can copy the raw bytes out if it
// wants to.
constexpr std::size_t kSlot0NameOffset = 0x8000;
constexpr std::size_t kSlot0NameLen    = 8;

// LSDJ 9.4.0 header-quick-scan format version byte. The byte at
// 0x8100 must equal this for rackboy's v1 parser to accept the
// file. LSDJ uses the same byte in the decompressed song at
// 0x7FFF, but that one is only visible after the block-stream
// decompressor runs — 0x8100 lets us reject a wrong-version sav
// without touching any decompression code.
constexpr std::size_t kHeaderVersionOffset   = 0x8100;
constexpr std::uint8_t kHeaderVersionLsdj940 = 0x1B;

// Block-allocation table layout. The byte at
// `kBlockAllocTableOffset + i` names which project slot owns block
// `i`, for i in [0, kBlockAllocTableLen). A byte value in
// [0, 31] is a slot index; 0xFF is "unallocated". This is the
// authoritative source for "which block starts slot S?" —
// scanning the table and returning the lowest `i` where the byte
// equals `S` matches LSDJ's own chain-start convention and the
// reference Rust extractor at
// gb-lsdj-instrument-extractor/crates/lsdj-sav/src/project.rs.
//
// Mirrored from spike_parser.cpp::kBlockAllocStart (and also
// echoed in tests/common/fixture_loader.cpp). Exposed here (not
// just in the spike) because the production loader under
// rackboy-klf needs to look up the first block of the active slot
// without pulling a test-only helper.
constexpr std::size_t kBlockAllocTableOffset = 0x8141;
constexpr std::size_t kBlockAllocTableLen    = 191;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// Soft-fail return code for every function in this header. `Ok`
// means "the buffer passed every check this function runs"; every
// other value names exactly one failure reason. The caller is
// expected to map these to a user-facing error (log line, UI
// toast, audio-thread silence, …). Values are stable for the life
// of rackboy v1 so callers can log them as raw ints without fear
// of silent renumbering.
enum class HeaderError : int {
    Ok                = 0,

    // `buf == nullptr`. Every validation function rejects null
    // up front so the rest of the body can assume a non-null
    // pointer.
    NullBuffer        = 1,

    // `len != kSavSizeBytes`. We do not try to validate a
    // partial or oversized buffer — LSDJ never produces one, so
    // the size mismatch is the signal.
    SizeMismatch      = 2,

    // The two bytes at 0x813E are not 'jk'. Almost always means
    // the buffer is not an LSDJ sav at all.
    MagicMismatch     = 3,

    // The active-slot byte at 0x8140 is out of LSDJ's 0..31
    // range. A truly corrupt header.
    ActiveSlotOutOfRange = 4,

    // The active-slot byte is a valid LSDJ slot index but not
    // slot 0, and rackboy v1 only consumes slot 0. A future
    // multi-slot bead can relax this.
    ActiveSlotNotZero = 5,

    // The 0x8100 header version byte is not 0x1B. rackboy v1 is
    // pinned to LSDJ 9.4.0; a mismatch means either an older /
    // newer LSDJ release or a corrupted header.
    VersionMismatch   = 6,
};

// Pin the enumerator values at compile time. The header comment
// promises stability across rackboy v1 so log-replay tooling can
// log raw ints without fear of silent renumbering — the
// static_asserts below are what actually enforce that promise. A
// future reorder fires these at the same spot the reorder happens,
// which is the earliest possible signal.
static_assert(static_cast<int>(HeaderError::Ok)                   == 0,
              "HeaderError::Ok must stay at 0.");
static_assert(static_cast<int>(HeaderError::NullBuffer)           == 1,
              "HeaderError::NullBuffer must stay at 1.");
static_assert(static_cast<int>(HeaderError::SizeMismatch)         == 2,
              "HeaderError::SizeMismatch must stay at 2.");
static_assert(static_cast<int>(HeaderError::MagicMismatch)        == 3,
              "HeaderError::MagicMismatch must stay at 3.");
static_assert(static_cast<int>(HeaderError::ActiveSlotOutOfRange) == 4,
              "HeaderError::ActiveSlotOutOfRange must stay at 4.");
static_assert(static_cast<int>(HeaderError::ActiveSlotNotZero)    == 5,
              "HeaderError::ActiveSlotNotZero must stay at 5.");
static_assert(static_cast<int>(HeaderError::VersionMismatch)      == 6,
              "HeaderError::VersionMismatch must stay at 6.");

// LSDJ's active-slot byte is stored as 0..31 (32 slots total).
// Values >= 32 are definitively corrupt — the LSDJ writer never
// produces them, so any sav that does is not round-trippable with
// any LSDJ build. This constant is used by the active-slot range
// check and exposed so tests can exercise the boundary.
constexpr std::uint8_t kLsdjMaxSlotExclusive = 32;

// ---------------------------------------------------------------------------
// HeaderInfo — parsed snapshot of the rackboy-relevant header fields
// ---------------------------------------------------------------------------
//
// Populated by `read_header`. Fields are only meaningful when the
// accompanying HeaderError return value is `Ok`. `read_header` does
// NOT touch this struct on any non-Ok return, so the caller does
// not have to zero it beforehand.

struct HeaderInfo {
    // The byte at 0x8140 — 0..31 on any valid LSDJ sav. rackboy v1
    // only accepts 0, but we still surface the field so a later
    // multi-slot bead can read it.
    std::uint8_t active_slot;

    // The byte at 0x8100 — 0x1B for LSDJ 9.4.0. Exposed so a later
    // bead can gate behavior on other LSDJ releases without
    // re-reading the buffer.
    std::uint8_t header_version;

    // True iff the two bytes at 0x813E were 'jk'. Always true on
    // an Ok return (the check is fatal), but carried as a field
    // anyway so tests can CHECK the magic directly rather than
    // having to re-read the buffer themselves.
    bool         magic_ok;
};

// POD contract — matches the rackboy_sav_types.h convention. A
// future refactor that adds a non-trivial member to HeaderInfo
// (e.g. `std::string note`) fires these at compile time rather
// than waiting for a mysterious memcpy-path bug downstream. See
// ~/.claude/research/cpp-pod-trivially-copyable-audio.md for the
// modern C++17 POD rules.
static_assert(std::is_trivially_copyable<HeaderInfo>::value,
              "HeaderInfo must stay trivially copyable to match the "
              "rackboy_sav_types.h POD convention.");
static_assert(std::is_standard_layout<HeaderInfo>::value,
              "HeaderInfo must stay standard-layout so host + Daisy "
              "agree on field offsets.");

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Validate a .sav buffer against every rackboy v1 container-level
// check: size, magic, active-slot range, active-slot == 0, and
// LSDJ 9.4.0 version byte. Returns `Ok` on full success; any
// earlier check that fails short-circuits and returns the matching
// error. `buf` may be null (returns `NullBuffer`) — the function
// is safe against nulls and over-/under-sized buffers.
//
// This function is the zero-out-param convenience overload of
// `read_header` below; the two share an implementation. Use this
// one when the caller only needs a yes/no answer and does not
// care about the parsed `HeaderInfo`.
//
// Noexcept and allocation-free. Intended to be called from the UI
// thread as part of a "load .sav" action; cheap enough (a handful
// of byte reads) to also run on every save-file path notification
// in a file watcher.
HeaderError validate_header(const std::uint8_t* buf,
                            std::size_t         len) noexcept;

// Like `validate_header` but additionally populates `out` with the
// parsed header fields on success. On failure, `out` is not
// touched. Passing `out == nullptr` is legal and equivalent to
// `validate_header` (the function is a superset, not a pickier
// variant).
HeaderError read_header(const std::uint8_t* buf,
                        std::size_t         len,
                        HeaderInfo*         out) noexcept;

// Return a short human-readable string for a HeaderError value.
// The returned pointer addresses a static literal; the caller does
// NOT free it, and the pointer stays valid for the life of the
// program. Useful for logging / UI without every caller having to
// maintain its own switch statement.
const char* header_error_string(HeaderError e) noexcept;

// Scan the block-allocation table for the first block whose owner
// byte equals `slot`, returning its block index in [0,
// kBlockAllocTableLen). Returns `kBlockAllocTableLen` to signal
// "no block allocated to `slot`", which the caller should treat
// as a soft failure (slot is either unallocated or corrupt).
//
// `buf` must point to at least `kBlockAllocTableOffset +
// kBlockAllocTableLen` bytes (i.e. a full LSDJ `.sav`). The
// function does NOT validate the container — callers are expected
// to have run `validate_header` already, or to be tolerant of a
// "not found" result on a malformed buffer. A null `buf` returns
// `kBlockAllocTableLen` rather than dereferencing.
//
// Noexcept and allocation-free. Intended to be called from the UI
// thread as part of a "load .sav" action; cost is a single linear
// scan over 191 bytes.
std::size_t first_block_for_slot(const std::uint8_t* buf,
                                 std::uint8_t        slot) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_HEADER_H_
