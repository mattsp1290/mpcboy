// rackboy_sav/rackboy_sav_instruments.h
//
// Instrument block parser: walks the allocation map of a
// decompressed LSDJ song blob, pulls each allocated 16-byte
// instrument record into the matching per-channel pool of a
// rackboy::sav::RackboyBank, and classifies by the LSDJ type byte.
// Bead rackboy-1ub.
//
// Input contract
// --------------
// The parser takes a *decompressed* LSDJ song blob — the 32 KiB
// (0x8000-byte) output of the block-stream decompressor, not the
// raw 128 KiB .sav file. Decompression lives in the sibling
// `rackboy_sav_compression` module (bead rackboy-cbo). Every byte
// offset below is relative to the start of that decompressed
// blob, and matches the offsets already exercised by the SPIKE-4
// parser at rackboy_sav/spike/spike_parser.cpp:
//
//   0x2040..0x2080   64-byte instrument allocation map.
//                    Byte at offset 0x2040 + slot is 0x01 when
//                    slot is allocated, 0x00 when unallocated.
//   0x3080 + slot*16 16-byte instrument record. Byte 0 is the
//                    LSDJ type byte (0x00 Pulse, 0x01 Wave,
//                    0x02 Kit, 0x03 Noise).
//
// Output contract
// ---------------
// The parser populates the per-channel pools of a caller-provided
// `rackboy::sav::RackboyBank`:
//
//   * Pulse (type 0x00) records are appended to `pulse1_pool` in
//     allocation-map order. A later bead will split between
//     `pulse1_pool` and `pulse2_pool` by walking the decompressed
//     song's chains / phrases to see which channel each instrument
//     is actually played on. Until that bead lands, v1 treats
//     every Pulse as Pulse1; documented in the channel-split
//     comment on `parse_instruments` below.
//
//   * Wave  (type 0x01) records are appended to `wave_pool`.
//
//   * Kit   (type 0x02) records are SKIPPED. LSDJ kits reference
//     PCM sample data in ROM, which rackboy v1 does not model.
//     Skipping a kit slot is not an error — the kit slots simply
//     don't appear in any of the per-channel pools, and the
//     audio thread plays silence on any control-to-index lookup
//     that lands on an unallocated slot (per the RackboyBank
//     dense-prefix contract).
//
//   * Noise (type 0x03) records are appended to `noise_pool`.
//
//   * Any other type byte is classified as `UnknownType` and
//     returned as an error — v1 does not silently drop a byte it
//     does not understand.
//
// The parser:
//   * **resets the bank first** via `bank_reset` so any stale
//     content the caller left in the bank is discarded;
//   * **writes the per-channel `*_count` fields** in sync with the
//     pool growth, so the dense-prefix invariant holds;
//   * **copies the full 16-byte record verbatim** into
//     `RackboyInstrument::raw` (the same memcpy the POD contract
//     promises). Per-byte field decoding (envelope, transpose,
//     vibrato depth, table reference) is a concern of the audio-
//     thread voice code, not this parser. See the per-type byte
//     layout comment on `RackboyInstrument` in
//     rackboy_sav_types.h.
//
// What the bead description *says* vs. what this module does
// ----------------------------------------------------------
// The rackboy-1ub description lists "extract NRxx initial
// register set, transpose, vibrato depth, envelope, table
// reference" as work items. Those are all fields *inside* the
// 16-byte raw record — there is no separate struct to decode
// into, because the `RackboyInstrument` struct already stores the
// record byte-for-byte and the voice code reads each field by
// offset. So "extract" in this module means "copy the 16 bytes
// verbatim"; the decode happens at audio-thread playback time.
// That matches the design comment in
// `rackboy_sav_types.h::RackboyInstrument` and is how the spike
// parser already works.
//
// Invariants
// ----------
// After a successful `parse_instruments` call, the following
// invariants hold on `out_bank`:
//
//   1. `pulse1_count + wave_count + noise_count ==` (number of
//      allocated slots that parsed as Pulse/Wave/Noise).
//   2. `pulse1_count <= kMaxInstrumentsPerChannel`, and the same
//      for `wave_count` / `noise_count`.
//   3. `pulse2_count == 0` (reserved for the chain-based split
//      bead; see above).
//   4. Every valid slot index for channel C has its 16-byte
//      record at `*_pool[i]` for `i < *_count`. Every slot index
//      `i >= *_count` is fully zeroed (from `bank_reset`).
//   5. `loaded` is set to `true` as the last step, so an audio
//      thread that races the swap observes either a fully-reset
//      bank (from the initial `bank_reset` on an Err return) or a
//      fully-populated one (on Ok).
//
// Soft-fail contract
// ------------------
// Every failure mode returns a dedicated `InstrumentsError`
// enumerator and (with one exception) leaves `out_bank` in a
// freshly-reset state — never in a half-populated one. A caller
// that hits an error can hand the reset bank to the audio thread
// and expect it to output silence, with no need for additional
// cleanup.
//
// The one exception is `NullOutBank`: when the caller passes a
// null bank pointer the parser returns immediately without
// touching any memory, because there is no bank to reset. Every
// other error code (NullSong, SongTooShort, AllocMapInvalid,
// UnknownType, ChannelPoolFull) leaves `*out_bank` in the
// freshly-reset state.
//
// The parser is `noexcept` and performs no allocation. Every
// read is bounds-checked against `song_len`.

#ifndef RACKBOY_SAV_RACKBOY_SAV_INSTRUMENTS_H_
#define RACKBOY_SAV_RACKBOY_SAV_INSTRUMENTS_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "rackboy_sav_compression.h"  // compression::kSongSize (canonical 0x8000)
#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// Decompressed-song layout constants
//
// Mirrored from rackboy_sav/spike/spike_parser.cpp. Per the
// "Mirror policy: spike-vs-production layout constants" section
// of docs/DECISIONS.md D-007, these constants are intentionally
// duplicated to preserve the spike's independence as the SPIKE-4
// acceptance oracle. Spike is canonical; any change here must
// land in the same commit as the matching edit in
// rackboy_sav/spike/spike_parser.cpp.
// ---------------------------------------------------------------------------

// Size (in bytes) of a full decompressed LSDJ song. Aliased to
// `rackboy::sav::compression::kSongSize` so the canonical 0x8000
// literal lives in one place inside production; per D-007 the
// compression-side name preserves the Rust port's
// `compression.rs::SONG_SIZE` symbol verbatim, while this header
// keeps the more self-documenting `kDecompressedSongSizeBytes`
// for higher-level callers. The spike-side mirror at
// `rackboy_sav/spike/spike_parser.cpp::kExpectedSongSize` is a
// separate independent `0x8000` literal per D-007's mirror policy
// (the spike is the SPIKE-4 acceptance oracle).
constexpr std::size_t kDecompressedSongSizeBytes = compression::kSongSize;  // 32 KiB

// Instrument allocation map: 64 bytes at [0x2040, 0x2080). Byte at
// `kInstrumentAllocMapOffset + slot` is `kInstrumentAllocated`
// when slot is in use and `kInstrumentUnallocated` otherwise.
constexpr std::size_t kInstrumentAllocMapOffset = 0x2040;
constexpr std::size_t kInstrumentAllocMapLen    = 64;
constexpr std::uint8_t kInstrumentAllocated     = 0x01;
constexpr std::uint8_t kInstrumentUnallocated   = 0x00;

// Instrument parameter area: 16 bytes per slot starting at 0x3080.
constexpr std::size_t kInstrumentParamsOffset = 0x3080;
constexpr std::size_t kInstrumentSlotStride   = kInstrumentRecordBytes;
// Total number of instrument slots = allocation map length. All
// 64 records fit in `[kInstrumentParamsOffset,
//  kInstrumentParamsOffset + 64 * 16) == [0x3080, 0x3480)`.
constexpr std::size_t kTotalInstrumentSlots =
    kInstrumentAllocMapLen;
constexpr std::size_t kInstrumentParamsEnd =
    kInstrumentParamsOffset
    + kTotalInstrumentSlots * kInstrumentSlotStride;

static_assert(kInstrumentParamsEnd <= kDecompressedSongSizeBytes,
              "Instrument param area must fit inside the "
              "decompressed song blob.");
static_assert(kInstrumentAllocMapOffset + kInstrumentAllocMapLen
              <= kDecompressedSongSizeBytes,
              "Instrument alloc map must fit inside the "
              "decompressed song blob.");
static_assert(kInstrumentAllocMapLen <= kMaxInstrumentsPerChannel,
              "Per-channel pool must be at least as large as the "
              "alloc map so a worst-case 'every slot is one type' "
              "song fits without tripping the ChannelPoolFull "
              "defensive check in parse_instruments.");

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// Soft-fail return code for `parse_instruments`. `Ok` means every
// allocated slot was classified and copied into the bank. Every
// other value names exactly one failure reason; the bank is
// bank_reset() on failure so the caller can still hand it to the
// audio thread for silent playback.
//
// Enumerator values are stable across rackboy v1 for the same
// log-replay reason `HeaderError` uses: callers may log them as
// raw ints. The `static_assert` block under the enum pins them at
// compile time.
enum class InstrumentsError : int {
    Ok               = 0,

    // `song == nullptr`.
    NullSong         = 1,

    // `out_bank == nullptr`.
    NullOutBank      = 2,

    // `song_len < kDecompressedSongSizeBytes`. Oversized buffers
    // are accepted — only the first 32 KiB are read.
    SongTooShort     = 3,

    // One of the 64 alloc-map bytes is neither 0x00 nor 0x01.
    // LSDJ never produces such a byte; it is a corrupt song or a
    // buffer from an incompatible format.
    AllocMapInvalid  = 4,

    // An allocated slot's type byte is not one of the four LSDJ
    // type bytes (0x00 Pulse, 0x01 Wave, 0x02 Kit, 0x03 Noise).
    // Kit is *not* an error — it is a deliberate v1 skip. Any
    // other value fires this.
    UnknownType      = 5,

    // An allocated slot's channel pool is already full. Can only
    // happen if a song somehow declares more than
    // `kMaxInstrumentsPerChannel == 64` instruments for one
    // channel, which LSDJ 9.4.0 cannot produce (the total cap is
    // 64 across ALL channels). Included for defensive completeness.
    ChannelPoolFull  = 6,
};

static_assert(static_cast<int>(InstrumentsError::Ok)              == 0,
              "InstrumentsError::Ok must stay at 0.");
static_assert(static_cast<int>(InstrumentsError::NullSong)        == 1,
              "InstrumentsError::NullSong must stay at 1.");
static_assert(static_cast<int>(InstrumentsError::NullOutBank)     == 2,
              "InstrumentsError::NullOutBank must stay at 2.");
static_assert(static_cast<int>(InstrumentsError::SongTooShort)    == 3,
              "InstrumentsError::SongTooShort must stay at 3.");
static_assert(static_cast<int>(InstrumentsError::AllocMapInvalid) == 4,
              "InstrumentsError::AllocMapInvalid must stay at 4.");
static_assert(static_cast<int>(InstrumentsError::UnknownType)     == 5,
              "InstrumentsError::UnknownType must stay at 5.");
static_assert(static_cast<int>(InstrumentsError::ChannelPoolFull) == 6,
              "InstrumentsError::ChannelPoolFull must stay at 6.");

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse the instrument block of a decompressed LSDJ song blob
// into a RackboyBank.
//
// Parameters:
//   song       — pointer to the first byte of the decompressed
//                song. The function reads at most
//                `kDecompressedSongSizeBytes` bytes starting here.
//                May be null (returns `NullSong`).
//   song_len   — length of the `song` buffer. Must be at least
//                `kDecompressedSongSizeBytes`. Larger buffers are
//                accepted; only the first 32 KiB are touched.
//   out_bank   — destination bank. May NOT be null; passing null
//                returns `NullOutBank` without touching any
//                memory. On entry the contents are discarded
//                (the function calls `bank_reset` before walking
//                the song). On a successful return the per-
//                channel pools are populated and
//                `loaded == true`. On every failure other than
//                `NullOutBank` the bank is left in the reset
//                state and `loaded == false`.
//
// Returns: `InstrumentsError::Ok` on success, one of the other
// enumerators on failure.
//
// Channel classification note: every Pulse instrument (type 0x00)
// lands in `pulse1_pool` for rackboy v1. Splitting Pulse1 vs
// Pulse2 requires walking the song's chain / phrase blocks to see
// which audio channel actually plays each instrument; that work
// lives in a future bead (tracked separately from rackboy-1ub).
// Until that bead lands, `pulse2_count` is always 0 after a
// successful parse.
//
// Safety / threading: `noexcept`; performs no allocation; all
// reads are bounds-checked against `song_len`. Intended to be
// called from the UI / parser thread; the populated bank is
// subsequently handed to the audio thread via the SPSC swap in
// rackboy_core::BankSlot (bead rackboy-2i8).
InstrumentsError parse_instruments(const std::uint8_t* song,
                                   std::size_t         song_len,
                                   RackboyBank*        out_bank) noexcept;

// Return a short human-readable string for an InstrumentsError.
// The returned pointer addresses a static literal; the caller does
// NOT free it and the pointer is valid for the life of the program.
const char* instruments_error_string(InstrumentsError e) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_INSTRUMENTS_H_
