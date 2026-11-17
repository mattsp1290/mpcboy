// rackboy_sav/rackboy_sav_types.h
//
// POD schema for the audio-thread-owned rackboy bank: per-channel
// instrument pools, a shared LSDJ table pool, and a shared CH3 wave
// RAM frame pool. Bead rackboy-3hk.
//
// The bank is parsed off the audio thread (see the separate
// rackboy_sav parser beads and rackboy_sav/spike/ prior work) and
// then handed to the audio thread via a single-producer /
// single-consumer pointer swap (rackboy-2i8). The audio thread only
// ever sees a fully-populated, immutable bank. That contract is why
// every struct in this header must be:
//
//   * trivially copyable — memcpy-able across the SPSC swap boundary;
//   * standard layout — host unit tests and the Daisy target agree
//     on field offsets;
//   * free of allocations, STL containers, or virtual methods;
//   * fixed-size — no pointers to external storage, no "dynamic"
//     arrays. The whole bank fits in a single struct the caller
//     places in DSY_SDRAM_BSS on Daisy, or in a plain static on VCV.
//
// The static_asserts at the end of this header enforce the POD
// contract at compile time. Adding a non-trivial member will fail
// the build loudly rather than wait for a runtime memcpy surprise.
//
// Scope note: this header defines the *types* only. Parsing, fixture
// loading, and the audio-thread swap handshake are tracked by other
// beads (rackboy-2i8 bank copy-in, the rackboy_sav top-level parser
// at rackboy-43y / rackboy-1ub / rackboy-u4v, and the populated
// rackboy_v1.sav work under rackboy-pj3).
//
// The per-instrument record layout and the wave / table pool sizes
// are all sourced from the binding wire contract in
// docs/COMPATIBILITY.md (the "extractor -> rackboy" format spec) and
// cross-checked against the LSDJ 9.4.0 references in
// docs/LSDJ_REFERENCE.md and the offset constants already live in
// rackboy_sav/spike/spike_parser.cpp.

#ifndef RACKBOY_SAV_RACKBOY_SAV_TYPES_H_
#define RACKBOY_SAV_RACKBOY_SAV_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rackboy {
namespace sav {

// ---------------------------------------------------------------------------
// Pool / table sizing constants
// ---------------------------------------------------------------------------

// Maximum number of instruments per channel in a rackboy bank. LSDJ
// actually caps the *total* instrument count at 64, but in the worst
// case all 64 may belong to a single channel, so every per-channel
// pool must be sized to the full 64 slots. The wasted slots on the
// other three channels (up to 192 in the pathological "all Pulse1"
// case) are explicitly accepted in exchange for a lock-free
// constant-time pool lookup on the audio thread.
constexpr std::size_t kMaxInstrumentsPerChannel = 64;

// Size (in bytes) of one on-disk LSDJ instrument record. Mirrored
// from spike_parser.cpp::kInstrumentSize (mirror policy in
// docs/DECISIONS.md D-007); cross-referenced against the binding
// wire contract at docs/COMPATIBILITY.md.
constexpr std::size_t kInstrumentRecordBytes = 16;

// Maximum number of LSDJ tables in a rackboy bank. LSDJ stores up to
// 32 tables as a shared pool; instruments reference them by slot
// index. See the 32-byte table allocation map at .sav offset 0x2020
// (rackboy_sav/spike/spike_parser.cpp `kTableAllocMapLen`).
constexpr std::size_t kMaxTables = 32;

// Maximum number of 16-byte CH3 wave frames in a rackboy bank.
// docs/COMPATIBILITY.md pins `WAVE_COUNT = 255` (the LSDJ wire
// sentinel `0xFF` reserves slot 255 as "no wave"). The
// decompressed song holds the shared wave bank at offset 0x6000 with
// one 16-byte frame per slot; CH3 Wave instruments reference frames
// by index (per-instrument `wave_index` byte). We pre-allocate all
// 255 slots so the audio thread's lookup is pointer-free.
constexpr std::size_t kMaxWaveFrames = 255;

// Every LSDJ table has exactly 16 rows. Duplicated from
// rackboy_core/rackboy_voice.h's kTableRowCount so this header does
// not have to depend on rackboy_core — the two constants are pinned
// to the same LSDJ 9.4.0 value in docs/LSDJ_REFERENCE.md.
constexpr std::size_t kTableRowCount = 16;

// A CH3 wave frame is 32 4-bit samples = 16 bytes. This matches the
// Game Boy CH3 wave RAM ($FF30..$FF3F), per Pan Docs §Audio, §CH3
// Wave Output Channel.
constexpr std::size_t kWaveFrameBytes = 16;

// ---------------------------------------------------------------------------
// LSDJ instrument type bytes (byte 0 of each 16-byte record)
// ---------------------------------------------------------------------------
// Mirrored from spike_parser.cpp::kExpectedInstruments (mirror
// policy in docs/DECISIONS.md D-007); cross-referenced against
// docs/COMPATIBILITY.md instrument type table.
constexpr std::uint8_t kInstrumentTypePulse = 0x00;  // CH1 or CH2
constexpr std::uint8_t kInstrumentTypeWave  = 0x01;  // CH3
constexpr std::uint8_t kInstrumentTypeKit   = 0x02;  // out of scope for v1
constexpr std::uint8_t kInstrumentTypeNoise = 0x03;  // CH4

// ---------------------------------------------------------------------------
// Sentinels
// ---------------------------------------------------------------------------

// "This instrument does not reference a table." Stored anywhere a
// table slot index is cached. `0xFF` matches LSDJ's own wire-level
// sentinel convention from docs/COMPATIBILITY.md.
constexpr std::uint8_t kNoTable = 0xFF;

// "This instrument does not reference a wave frame." Matches LSDJ's
// wire sentinel: slot `0xFF` is reserved and unused
// (docs/COMPATIBILITY.md `WAVE_COUNT = 255`, "slot 0xFF is reserved").
constexpr std::uint8_t kNoWaveFrame = 0xFF;

// Compile-time sanity: every real slot index must be strictly below
// the sentinel so there is no ambiguity. Checked at the constant
// definition rather than as a runtime doctest so a future bump to
// kMaxTables or kMaxWaveFrames that would create a collision fails
// the build immediately.
static_assert(kMaxTables     < static_cast<std::size_t>(kNoTable),
              "kMaxTables must fit below kNoTable so no real slot collides "
              "with the 'no table' sentinel.");
static_assert(kMaxWaveFrames <= static_cast<std::size_t>(kNoWaveFrame),
              "kMaxWaveFrames must fit at or below kNoWaveFrame so no real "
              "slot collides with the 'no wave frame' sentinel. LSDJ's wire "
              "format reserves 0xFF exactly for the sentinel, so equality "
              "on the upper bound is acceptable here.");

// ---------------------------------------------------------------------------
// RackboyInstrument — one GB instrument's 16-byte LSDJ body record
// ---------------------------------------------------------------------------
//
// Each LSDJ instrument is a 16-byte record at offset
// `0x3080 + slot_index * 16` in the decompressed song blob. The parser
// memcpys these bytes verbatim into `RackboyInstrument::raw`; the
// audio thread decodes individual bytes against the per-type layout
// documented in docs/COMPATIBILITY.md.
//
// Why raw bytes instead of named fields? Three reasons:
//
//   1. **Faithful wire contract.** The parser's job is trivially
//      `memcpy(dst.raw, src + 0x3080 + slot*16, 16)` — no field-by-
//      field decoding, no risk of losing a byte, and any later
//      extension LSDJ makes to the 16-byte record is automatically
//      captured.
//
//   2. **POD simplicity.** A single fixed-size byte array is the
//      strictest possible POD; no alignment surprises, no padding
//      between sub-fields, no type drift.
//
//   3. **Audio-thread access is still O(1).** Decoding a single byte
//      out of `raw[k]` is a load + mask, identical in cost to reading
//      a named struct member that would have been at the same offset.
//
// Per-type byte layout (from docs/COMPATIBILITY.md §"Instrument body"):
//
//   Byte 0  = type_byte (0x00 Pulse, 0x01 Wave, 0x02 Kit, 0x03 Noise)
//
//   Pulse (CH1 / CH2):
//     Byte 1  = envelope            (NR12 / NR22 initial byte)
//     Byte 2  = phase               (reserved)
//     Byte 3  = length              (bit 6 = unlimited)
//     Byte 4  = sweep               (NR10 byte, CH1 only; ignored on CH2)
//     Byte 5  = transpose_table     (bit 5 = auto-transpose, bit 4 = table enable)
//     Byte 6  = table slot          (0..31, or kNoTable)
//     Byte 7  = duty_finetune_pan   (bits 6-7 duty, 2-5 finetune, 0-1 pan)
//     Byte 8+ = vendor-reserved / zero
//
//   Wave (CH3):
//     Byte 1  = volume              (NR32 output level: bits 6-5 of NR32)
//     Byte 2  = loop_config
//     Byte 3  = length
//     Byte 4  = reserved
//     Byte 5  = reserved
//     Byte 6  = table slot
//     Byte 7  = reserved
//     Byte 8  = wave_index          (0..254 into RackboyBank::waves, or 0xFF)
//     Byte 9  = play_mode           (bits 0-1 playback mode)
//     Byte 10 = speed
//     Byte 11+ = vendor-reserved / zero
//
//   Noise (CH4):
//     Byte 1  = envelope            (NR42 initial byte)
//     Byte 2  = stability           (bit 0 = LSDJ extended-stability bit)
//     Byte 3  = length              (bit 6 = unlimited)
//     Byte 4  = shape               (NR43 initial byte)
//     Byte 5+ = vendor-reserved / zero
//
// These offsets are the audio thread's contract. Don't rewrite the
// struct to use named fields without also keeping the raw[16] view
// for the parser's memcpy path.
//
// An empty / unused slot has `raw[0] == 0x00` and all other bytes 0.
// Pool occupancy is determined by the parent `RackboyBank::*_count`
// prefix — slot index `i` is valid iff `i < *_count` for its channel
// pool. There is deliberately NO per-instrument occupancy flag; the
// dense-prefix invariant is cheaper and harder to get out of sync
// than maintaining a separate boolean per slot.
struct RackboyInstrument {
    std::uint8_t raw[kInstrumentRecordBytes];
};

// ---------------------------------------------------------------------------
// RackboyTableRow / RackboyTable — LSDJ table, 16 rows
// ---------------------------------------------------------------------------
//
// Each LSDJ table row has a signed transpose offset and two command
// slots. The command letter codes and parameter layouts follow LSDJ
// 9.4.0 per docs/LSDJ_REFERENCE.md; the specific per-command
// semantics are handled by rackboy_table::interpret_row (tracked by
// rackboy-tj3) and are not decoded here — this header only defines
// the storage.

struct RackboyTableRow {
    // Signed semitone offset from the instrument's base note. LSDJ
    // allows roughly ±48 semitones in practice; int8_t's [-128, 127]
    // range is plenty.
    std::int8_t transpose;

    // First and second command slots per row. Each is a raw
    // (letter_code, parameter) pair sourced from the .sav. The
    // letter code 0x00 means "no command in this slot".
    std::uint8_t cmd1;
    std::uint8_t cmd1_param;
    std::uint8_t cmd2;
    std::uint8_t cmd2_param;
    // MPCBOY LOCAL MOD: LSDJ table volume column. Valid only when has_vol is
    // set. WP11: parse_tables reads the envelope column at 0x1690 into vol and
    // sets has_vol = (vol != 0x00); a literal 0x00 volume cannot be told from an
    // empty cell in LSDJ data and imports as no volume.
    std::uint8_t vol;
    bool         has_vol;
};

// A table slot carries its own occupancy flag because LSDJ's table
// pool is addressed by slot index, not by a dense prefix — the
// instrument at slot 3 may reference table slot 17, leaving slots
// 0..16 as holes the parser never touches. The `occupied` flag lets
// downstream code tell "slot referenced but unused" from "slot
// referenced and valid".
struct RackboyTable {
    RackboyTableRow rows[kTableRowCount];
    bool            occupied;
};

// ---------------------------------------------------------------------------
// RackboyWaveFrame — one 16-byte CH3 wave RAM payload
// ---------------------------------------------------------------------------
//
// The audio-thread CH3 path writes these 16 bytes directly into
// $FF30..$FF3F (through the rackboy_apu shim, which owns the
// DAC-disable / write / DAC-enable ordering — see docs/LSDJ_REFERENCE
// the `W` command row and bead rackboy-9i0).

struct RackboyWaveFrame {
    std::uint8_t samples[kWaveFrameBytes];
    // Same rationale as RackboyTable::occupied — the wave frame pool
    // is addressed by index and contains holes.
    bool occupied;
};

// ---------------------------------------------------------------------------
// RackboyBank — the top-level audio-thread-owned bank struct
// ---------------------------------------------------------------------------
//
// Pre-allocated to the pathological maximum so the audio-thread path
// is O(1) / pointer-free / branch-light. The SPSC copy-in handshake
// (rackboy-2i8) populates one of these off-thread and hands the
// pointer to the audio thread via an atomic swap; the audio thread
// never touches memory it does not already own.
//
// Per-channel pools are **dense prefixes**: valid slot indices for
// channel C are exactly [0, C_count). Parsing compacts the pool as
// it walks the decompressed song so the audio thread can index
// directly with the output of the control-to-index hysteresis
// mapping without having to skip holes.

struct RackboyBank {
    // Per-channel instrument pools. Index into these with the
    // control-to-index hysteresis mapping
    // (rackboy_core/rackboy_control.h) and clamp to the matching
    // *_count below.
    RackboyInstrument pulse1_pool[kMaxInstrumentsPerChannel];
    RackboyInstrument pulse2_pool[kMaxInstrumentsPerChannel];
    RackboyInstrument wave_pool  [kMaxInstrumentsPerChannel];
    RackboyInstrument noise_pool [kMaxInstrumentsPerChannel];

    // Per-channel used count. Values in
    // [0, kMaxInstrumentsPerChannel]. A count of 0 means the channel
    // has an empty pool and must output silence (matches the
    // kNoInstrument sentinel returned by rackboy_core's hysteresis).
    std::uint8_t pulse1_count;
    std::uint8_t pulse2_count;
    std::uint8_t wave_count;
    std::uint8_t noise_count;

    // Shared LSDJ table pool. Referenced from an instrument's table
    // slot byte (see the RackboyInstrument layout comment). Unused
    // slots carry `occupied == false`.
    RackboyTable tables[kMaxTables];

    // Shared CH3 wave frame pool. Referenced from an instrument's
    // wave_index byte. Unused slots carry `occupied == false`.
    RackboyWaveFrame waves[kMaxWaveFrames];

    // Did this bank come from a successful .sav parse? A fresh /
    // reset bank starts with `loaded == false`; the audio thread
    // must output silence on every channel in that state. Set to
    // `true` by the off-thread parser once the bank is fully
    // populated, just before the SPSC pointer swap.
    bool loaded;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Zero every byte of an instrument record. The zero body implicitly
// carries `type_byte == kInstrumentTypePulse` per LSDJ's convention
// — callers that need to distinguish "empty slot" from "real pulse"
// use the parent bank's `*_count` prefix (empty slots live beyond
// the count).
inline void instrument_reset(RackboyInstrument& i) noexcept
{
    for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
        i.raw[k] = 0;
    }
}

// Zero every row of a table and mark it unoccupied.
inline void table_reset(RackboyTable& t) noexcept
{
    for (std::size_t row = 0; row < kTableRowCount; ++row) {
        t.rows[row].transpose  = 0;
        t.rows[row].cmd1       = 0;
        t.rows[row].cmd1_param = 0;
        t.rows[row].cmd2       = 0;
        t.rows[row].cmd2_param = 0;
        t.rows[row].vol        = 0;      // MPCBOY LOCAL MOD
        t.rows[row].has_vol    = false;  // MPCBOY LOCAL MOD
    }
    t.occupied = false;
}

// Zero every sample byte of a wave frame and mark it unoccupied.
inline void wave_frame_reset(RackboyWaveFrame& w) noexcept
{
    for (std::size_t i = 0; i < kWaveFrameBytes; ++i) {
        w.samples[i] = 0;
    }
    w.occupied = false;
}

// Reset the entire bank to "fresh / power-on" state: every pool slot
// zeroed, every count 0, every table + wave frame slot unoccupied,
// `loaded == false`. After this call the audio thread will silence
// every channel. Called during module construction, on explicit bank
// unload, and as the starting state of the off-thread parser before
// it fills in a new bank.
inline void bank_reset(RackboyBank& b) noexcept
{
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        instrument_reset(b.pulse1_pool[i]);
        instrument_reset(b.pulse2_pool[i]);
        instrument_reset(b.wave_pool[i]);
        instrument_reset(b.noise_pool[i]);
    }
    b.pulse1_count = 0;
    b.pulse2_count = 0;
    b.wave_count   = 0;
    b.noise_count  = 0;

    for (std::size_t t = 0; t < kMaxTables; ++t) {
        table_reset(b.tables[t]);
    }
    for (std::size_t w = 0; w < kMaxWaveFrames; ++w) {
        wave_frame_reset(b.waves[w]);
    }

    b.loaded = false;
}

// ---------------------------------------------------------------------------
// Compile-time POD contract enforcement
// ---------------------------------------------------------------------------
//
// Adding a non-trivial member, a user-declared ctor, or a virtual
// method to any of these structs will fail at least one of these
// static_asserts and halt the build — long before a mysterious
// audio-thread bug shows up. The research cache at
// ~/.claude/research/cpp-pod-trivially-copyable-audio.md explains
// why is_trivially_copyable + is_standard_layout is the modern-C++
// replacement for the deprecated "POD" label.

static_assert(std::is_trivially_copyable<RackboyInstrument>::value,
              "RackboyInstrument must stay trivially copyable for the "
              "SPSC bank copy-in.");
static_assert(std::is_standard_layout<RackboyInstrument>::value,
              "RackboyInstrument must stay standard-layout so host + Daisy "
              "agree on field offsets.");
static_assert(sizeof(RackboyInstrument) == kInstrumentRecordBytes,
              "RackboyInstrument must be exactly kInstrumentRecordBytes "
              "wide so a memcpy from the decompressed song lands in the "
              "expected layout without padding surprises.");

static_assert(std::is_trivially_copyable<RackboyTableRow>::value,
              "RackboyTableRow must stay trivially copyable.");
static_assert(std::is_standard_layout<RackboyTableRow>::value,
              "RackboyTableRow must stay standard-layout.");

// MPCBOY LOCAL MOD: vol + has_vol grew the row from 5 to 7 bytes.
static_assert(sizeof(RackboyTableRow) == 7,
              "RackboyTableRow is transpose + 2 commands + 2 params + vol + has_vol.");
static_assert(std::is_trivially_copyable<RackboyTable>::value,
              "RackboyTable must stay trivially copyable.");
static_assert(std::is_standard_layout<RackboyTable>::value,
              "RackboyTable must stay standard-layout.");

static_assert(std::is_trivially_copyable<RackboyWaveFrame>::value,
              "RackboyWaveFrame must stay trivially copyable.");
static_assert(std::is_standard_layout<RackboyWaveFrame>::value,
              "RackboyWaveFrame must stay standard-layout.");

static_assert(std::is_trivially_copyable<RackboyBank>::value,
              "RackboyBank must stay trivially copyable so the SPSC "
              "copy-in handshake can memcpy it as a whole.");
static_assert(std::is_standard_layout<RackboyBank>::value,
              "RackboyBank must stay standard-layout.");

}  // namespace sav
}  // namespace rackboy

#endif  // RACKBOY_SAV_RACKBOY_SAV_TYPES_H_
