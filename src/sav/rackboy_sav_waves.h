// rackboy_sav/rackboy_sav_waves.h
//
// CH3 wave frame pool parser: walks the 255 × 16-byte wave
// frame pool of a decompressed LSDJ song blob and copies each
// frame into the matching slot of a
// rackboy::sav::RackboyBank. Bead rackboy-ntq.
//
// What a "wave frame" is
// ----------------------
// LSDJ CH3 instruments reference a 16-byte waveform that holds
// 32 4-bit samples. On playback the audio thread writes those
// 16 bytes directly into the Game Boy's CH3 wave RAM at
// $FF30..$FF3F (with the DAC disable / write / DAC re-enable
// dance handled by the rackboy_apu shim — see bead rackboy-9i0).
// Pan Docs §Audio §CH3 Wave Output Channel documents the wave
// RAM layout.
//
// Pool layout on disk
// -------------------
// The decompressed LSDJ song carries a shared pool of 255 wave
// frames starting at byte offset 0x6000, laid out flat as 255
// consecutive 16-byte records. There is NO separate allocation
// map — unlike the instrument pool (0x2040) or the table pool
// (0x2020), the wave pool is entirely positional. See
// docs/COMPATIBILITY.md §"Decompressed song layout" and the
// `WAVE_COUNT = 255` / `WAVE_LEN = 16` constants in that doc.
//
// Per the LSDJ wire format, slot index 0xFF is reserved and
// unused; `kMaxWaveFrames` is therefore pinned at 255 and the
// `kNoWaveFrame` sentinel in rackboy_sav_types.h uses 0xFF.
//
// Occupancy inference (no alloc map)
// ----------------------------------
// Because there is no alloc map, the parser infers occupancy
// by content: a frame whose 16 bytes are all zero is treated
// as "unused" and left unoccupied, with its sample bytes
// zeroed. A frame with any non-zero byte is treated as "in
// use" and copied verbatim into `waves[slot].samples`, with
// `waves[slot].occupied` set to true.
//
// The heuristic is safe because:
//
//   * The decompressor zero-fills any wave slots that weren't
//     touched by the source .sav (short decompressed output is
//     padded to 32 KiB with 0x00; see docs/COMPATIBILITY.md
//     §"Compression format"), so every genuinely-unused slot
//     reads as all zeros here.
//   * An all-zero wave frame is sonically silent — every
//     4-bit sample is DC 0. Marking it unoccupied means the
//     audio thread can skip it during a future "enumerate
//     populated wave frames" pass without losing any audible
//     content.
//   * If a future LSDJ author actually crafts an all-zero wave
//     (silent DC), marking it unoccupied still produces the
//     correct audible result: the CH3 voice will play silence
//     one way or the other.
//
// Input contract
// --------------
//
//   song     — pointer to the first byte of the decompressed
//              song. Reads at most `kDecompressedSongSizeBytes`.
//              May be null (returns `NullSong`).
//   song_len — length of `song`. Must be >=
//              `kDecompressedSongSizeBytes`. Larger buffers
//              are accepted; only the first 32 KiB are read.
//   bank     — destination bank. May NOT be null.
//
// Output contract
// ---------------
// The parser updates the `waves[]` array of `bank` in place:
// slots whose on-disk bytes contain any non-zero have their
// samples populated and `occupied = true`; all-zero slots are
// cleared (samples zeroed, `occupied = false`). The parser
// does NOT touch per-channel instrument pools, the `tables[]`
// array, or the `loaded` flag — it is expected to run AFTER
// `parse_instruments` and alongside `parse_tables` in a
// combined pipeline.
//
// Soft-fail contract
// ------------------
// Every failure mode resets the `waves[]` sub-array via
// `wave_frame_reset` on each slot, so the bank's wave
// sub-array is always in a known-clean state on return.
// Instrument pools, `tables[]`, and the `loaded` flag are
// NEVER touched. The sole exception is `NullOutBank`: when the
// caller passes a null bank pointer the parser returns
// immediately without touching any memory, because there is
// no bank to reset.
//
// `noexcept`; no allocation; every `song[offset]` read is
// bounds-proven at compile time against
// `kDecompressedSongSizeBytes`.

#ifndef RACKBOY_SAV_RACKBOY_SAV_WAVES_H_
#define RACKBOY_SAV_RACKBOY_SAV_WAVES_H_

#include <cstddef>
#include <cstdint>

#include "rackboy_sav_instruments.h"  // kDecompressedSongSizeBytes
#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// Decompressed-song wave-pool layout constants
//
// MIRRORED against docs/COMPATIBILITY.md §"Decompressed song
// layout": the Wave frames row pins `Offset 0x6000`,
// `Count 255`, `Stride 16 bytes` (total 4080 bytes), and the
// explicit `WAVE_COUNT = 255` callout below the table
// reserves slot 0xFF as the `kNoWaveFrame` sentinel. Keep in
// sync until a shared `rackboy_sav_layout.h` is extracted
// (see the duplication note on rackboy_sav_header.h /
// rackboy_sav_instruments.h / rackboy_sav_tables.h).
// ---------------------------------------------------------------------------

// Byte offset of the first wave frame inside the decompressed
// song. Subsequent frames are laid out consecutively with no
// gaps: slot `k` starts at `kWaveFramePoolOffset + k*16`.
constexpr std::size_t kWaveFramePoolOffset = 0x6000;

// Stride (bytes) between consecutive wave frame slots. Always
// `kWaveFrameBytes` — no padding.
constexpr std::size_t kWaveFrameSlotStride = kWaveFrameBytes;  // 16

// Total number of wave frame slots in the decompressed song.
// Must match `kMaxWaveFrames` in rackboy_sav_types.h so every
// on-disk slot has a matching in-memory slot.
constexpr std::size_t kTotalWaveFrameSlots = kMaxWaveFrames;  // 255

// Every read at
// `song[kWaveFramePoolOffset + k*kWaveFrameSlotStride + b]`
// with `k < kTotalWaveFrameSlots` and
// `b < kWaveFrameSlotStride` lies inside the decompressed-song
// blob. The parser's `song_len >= kDecompressedSongSizeBytes`
// runtime check turns these static_asserts into a complete
// bounds proof for every byte read in the implementation.
static_assert(kWaveFramePoolOffset
              + kTotalWaveFrameSlots * kWaveFrameSlotStride
              <= kDecompressedSongSizeBytes,
              "Wave frame pool must fit inside the "
              "decompressed song blob.");
static_assert(kTotalWaveFrameSlots == kMaxWaveFrames,
              "kTotalWaveFrameSlots must equal kMaxWaveFrames "
              "so every on-disk wave frame has a matching "
              "in-memory slot.");
static_assert(kWaveFrameSlotStride == kWaveFrameBytes,
              "kWaveFrameSlotStride must equal kWaveFrameBytes "
              "so the in-memory RackboyWaveFrame::samples "
              "layout matches the on-disk slot stride.");

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

// Soft-fail return code for `parse_waves`. Enumerator values
// stable across rackboy v1 for log-replay tooling (enforced by
// the `static_assert` block below). Matches the HeaderError /
// InstrumentsError / TablesError convention: a log-replay tool
// may print a raw int and deserialize it safely years later.
enum class WavesError : int {
    Ok            = 0,

    // `song == nullptr`.
    NullSong      = 1,

    // `out_bank == nullptr`.
    NullOutBank   = 2,

    // `song_len < kDecompressedSongSizeBytes`.
    SongTooShort  = 3,
};

static_assert(static_cast<int>(WavesError::Ok)           == 0,
              "WavesError::Ok must stay at 0.");
static_assert(static_cast<int>(WavesError::NullSong)     == 1,
              "WavesError::NullSong must stay at 1.");
static_assert(static_cast<int>(WavesError::NullOutBank)  == 2,
              "WavesError::NullOutBank must stay at 2.");
static_assert(static_cast<int>(WavesError::SongTooShort) == 3,
              "WavesError::SongTooShort must stay at 3.");

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse the CH3 wave frame pool of a decompressed LSDJ song
// blob into the `waves[]` array of a RackboyBank.
//
// Parameters:
//   song      — pointer to the first byte of the decompressed
//               song. Reads at most 32 KiB. May be null
//               (returns `NullSong`).
//   song_len  — length of `song`. Must be >=
//               `kDecompressedSongSizeBytes`. Larger buffers
//               are accepted; only the first 32 KiB are read.
//   out_bank  — destination bank. May NOT be null; passing
//               null returns `NullOutBank` without touching
//               any memory. The parser rewrites the `waves[]`
//               array in place but does NOT touch the per-
//               channel instrument pools, `tables[]`, or the
//               `loaded` flag — it is expected to run in a
//               combined pipeline with `parse_instruments`
//               and `parse_tables`.
//
// Returns: `WavesError::Ok` on success.
//
// Every on-disk `song[offset]` read is bounds-proven at
// compile time against `kDecompressedSongSizeBytes`; the only
// runtime guard is the `song_len` length check.
WavesError parse_waves(const std::uint8_t* song,
                       std::size_t         song_len,
                       RackboyBank*        out_bank) noexcept;

// Return a short human-readable string for a WavesError.
// Static-lifetime storage; callers do NOT free the returned
// pointer.
const char* waves_error_string(WavesError e) noexcept;

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_WAVES_H_
