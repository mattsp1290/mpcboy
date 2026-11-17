// rackboy_sav/rackboy_sav_compression.h
//
// LSDJ block-chain RLE decompressor. Originally the SPIKE-4
// spike port at `rackboy_sav/spike/compression.h`; promoted to a
// first-class rackboy_sav module under bead rackboy-cbo now that
// the VCV plugin's "Load LSDJ file..." path (bead rackboy-klf)
// depends on it unconditionally.
//
// This is a mechanical C++17 port of
// /Users/punk1290/git/gb-lsdj-instrument-extractor/crates/lsdj-sav/src/compression.rs
// at extractor commit 2f4c4540475c7ad041089f624f8a5b4b5abc6ddd. Matt owns
// both repositories; both the reference and this port are MIT-licensed.
// Per docs/DECISIONS.md D-007, the port is intentionally faithful — Rust
// function names, local variables, and control flow are preserved 1:1 so
// the two implementations can be cross-audited.
//
// Only `decompress` is ported. LSDJ recompression is out of scope for
// rackboy v1 (the module is read-only against user `.sav` files).
//
// No STL beyond <cstdint>, <cstddef>, <optional>, <string>, <vector>.

#ifndef RACKBOY_SAV_RACKBOY_SAV_COMPRESSION_H_
#define RACKBOY_SAV_RACKBOY_SAV_COMPRESSION_H_

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace rackboy::sav::compression {

// Decompressed song size: 32 KiB. (port of compression.rs::SONG_SIZE)
constexpr std::size_t kSongSize = 0x8000;
static_assert(kSongSize == 0x8000,
              "Decompressed song size is fixed at 0x8000 (32 KiB) by "
              "the LSDJ wire contract. If this genuinely needs to "
              "change, also update the spike-side mirror at "
              "rackboy_sav/spike/spike_parser.cpp::kExpectedSongSize "
              "in the same commit (per docs/DECISIONS.md D-007 "
              "mirror policy).");

// LSDJ .sav total size: 128 KiB. (port of sav.rs::SAV_SIZE)
constexpr std::size_t kSavSize = 0x20000;

// Block size in bytes. (port of compression.rs::BLOCK_SIZE)
constexpr std::size_t kBlockSize = 0x200;

// Compressed blocks start at this offset inside the .sav buffer.
// (port of compression.rs::BLOCKS_OFFSET)
constexpr std::size_t kBlocksOffset = 0x8200;

// Default wave pattern (16 bytes) expanded by the 0xE0 0xF0 <count> opcode.
// (port of compression.rs::DEFAULT_WAVE)
extern const std::uint8_t kDefaultWave[16];

// Default instrument pattern (16 bytes) expanded by the 0xE0 0xF1 <count>
// opcode. (port of compression.rs::DEFAULT_INSTRUMENT)
extern const std::uint8_t kDefaultInstrument[16];

// Decompress a song from the .sav buffer.
//
// `sav_data` must be at least `kSavSize` bytes. `start_block` is the
// 0-based index of the first block to read from. Blocks are located at
// `kBlocksOffset + block_id * kBlockSize`.
//
// On success returns a 32 KiB `std::vector<std::uint8_t>` containing the
// decompressed song. On failure returns `std::nullopt` and sets `err_out`
// to a human-readable error string describing the failure.
//
// (port of compression.rs::decompress)
std::optional<std::vector<std::uint8_t>> decompress(
    const std::uint8_t* sav_data,
    std::size_t sav_size,
    std::uint8_t start_block,
    std::string* err_out);

}  // namespace rackboy::sav::compression

#endif  // RACKBOY_SAV_RACKBOY_SAV_COMPRESSION_H_
