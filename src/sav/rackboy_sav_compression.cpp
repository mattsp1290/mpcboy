// rackboy_sav/rackboy_sav_compression.cpp
//
// LSDJ block-chain RLE decompressor — C++17 port. Promoted out of
// `rackboy_sav/spike/compression.cpp` under bead rackboy-cbo once
// the VCV plugin started linking it unconditionally.
//
// Reference: /Users/punk1290/git/gb-lsdj-instrument-extractor/crates/lsdj-sav/src/compression.rs
// at extractor commit 2f4c4540475c7ad041089f624f8a5b4b5abc6ddd. See
// rackboy_sav_compression.h for the port provenance and the D-007
// acceptance interpretation that motivates this port.
//
// Every branch here has a `// port of compression.rs:LLL` marker where
// the corresponding Rust line number isn't obvious from the structure.

#include "rackboy_sav_compression.h"

#include <cstdio>

namespace rackboy::sav::compression {

// port of compression.rs::DEFAULT_WAVE
const std::uint8_t kDefaultWave[16] = {
    0x8E, 0xCD, 0xCC, 0xBB, 0xAA, 0xA9, 0x99, 0x88,
    0x87, 0x76, 0x66, 0x55, 0x54, 0x43, 0x32, 0x31,
};

// port of compression.rs::DEFAULT_INSTRUMENT
const std::uint8_t kDefaultInstrument[16] = {
    0xA8, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x03, 0x00,
    0x00, 0xD0, 0x00, 0x00, 0x00, 0xF3, 0x00, 0x00,
};

namespace {

// Convenience: set *err to a formatted error string.
void set_err(std::string* err_out, const char* fmt, int arg0 = 0, int arg1 = 0) {
    if (!err_out) {
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg0, arg1);
    err_out->assign(buf);
}

// Check whether `chain` already contains `block_id`. port of the
// `switch_chain.contains(&next_block)` call at compression.rs:172.
bool chain_contains(const std::vector<std::size_t>& chain,
                    std::size_t block_id) {
    for (std::size_t id : chain) {
        if (id == block_id) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> decompress(
    const std::uint8_t* sav_data,
    std::size_t sav_size,
    std::uint8_t start_block,
    std::string* err_out) {
    // port of compression.rs:30
    if (sav_size < kSavSize) {
        set_err(err_out,
                "sav data too small: %d bytes (expected at least %d)",
                static_cast<int>(sav_size),
                static_cast<int>(kSavSize));
        return std::nullopt;
    }

    std::vector<std::uint8_t> output;
    output.reserve(kSongSize);

    std::size_t block_id = static_cast<std::size_t>(start_block);
    std::size_t pos = 0;  // position within current block

    // Cycle detection for block-switch chains (see compression.rs:42-51).
    std::vector<std::size_t> switch_chain;
    std::size_t last_output_len = 0;

    for (;;) {
        // port of compression.rs:56
        if (output.size() >= kSongSize) {
            break;
        }

        // port of compression.rs:60-71 — hard safety net against
        // pathological inputs that interleave emissions with switch chains.
        if (output.size() > kSongSize * 2) {
            set_err(err_out,
                    "decompressed output exceeded maximum expected size");
            return std::nullopt;
        }

        // port of compression.rs:73-77
        if (output.size() != last_output_len) {
            switch_chain.clear();
            last_output_len = output.size();
        }

        const std::size_t block_offset = kBlocksOffset + block_id * kBlockSize;
        // port of compression.rs:79
        if (block_offset + kBlockSize > sav_size) {
            set_err(err_out,
                    "block %d out of range (offset exceeds sav size)",
                    static_cast<int>(block_id));
            return std::nullopt;
        }

        // port of compression.rs:87
        if (pos >= kBlockSize) {
            set_err(err_out, "read past end of block without block switch");
            return std::nullopt;
        }

        const std::uint8_t byte = sav_data[block_offset + pos];
        pos += 1;

        if (byte == 0xC0) {
            // RLE or literal 0xC0 — port of compression.rs:96-123
            if (pos >= kBlockSize) {
                set_err(err_out, "truncated C0 escape");
                return std::nullopt;
            }
            const std::uint8_t next = sav_data[block_offset + pos];
            pos += 1;

            if (next == 0xC0) {
                // Literal 0xC0
                output.push_back(0xC0);
            } else {
                // RLE: repeat `next` byte `count` times
                if (pos >= kBlockSize) {
                    set_err(err_out, "truncated RLE count");
                    return std::nullopt;
                }
                const std::size_t count =
                    static_cast<std::size_t>(sav_data[block_offset + pos]);
                pos += 1;
                for (std::size_t i = 0; i < count; ++i) {
                    output.push_back(next);
                }
            }
        } else if (byte == 0xE0) {
            // Special commands — port of compression.rs:124-183
            if (pos >= kBlockSize) {
                set_err(err_out, "truncated E0 escape");
                return std::nullopt;
            }
            const std::uint8_t cmd = sav_data[block_offset + pos];
            pos += 1;

            if (cmd == 0xE0) {
                // Literal 0xE0
                output.push_back(0xE0);
            } else if (cmd == 0xFF) {
                // End of song
                break;
            } else if (cmd == 0xF0) {
                // Default wave × count
                if (pos >= kBlockSize) {
                    set_err(err_out, "truncated F0 count");
                    return std::nullopt;
                }
                const std::size_t count =
                    static_cast<std::size_t>(sav_data[block_offset + pos]);
                pos += 1;
                for (std::size_t i = 0; i < count; ++i) {
                    for (std::size_t j = 0; j < 16; ++j) {
                        output.push_back(kDefaultWave[j]);
                    }
                }
            } else if (cmd == 0xF1) {
                // Default instrument × count
                if (pos >= kBlockSize) {
                    set_err(err_out, "truncated F1 count");
                    return std::nullopt;
                }
                const std::size_t count =
                    static_cast<std::size_t>(sav_data[block_offset + pos]);
                pos += 1;
                for (std::size_t i = 0; i < count; ++i) {
                    for (std::size_t j = 0; j < 16; ++j) {
                        output.push_back(kDefaultInstrument[j]);
                    }
                }
            } else {
                // Block switch: continue reading from block `cmd`.
                const std::size_t next_block = static_cast<std::size_t>(cmd);
                if (chain_contains(switch_chain, next_block)) {
                    set_err(err_out,
                            "cyclic block-switch chain: block %d revisited without output progress",
                            static_cast<int>(next_block));
                    return std::nullopt;
                }
                switch_chain.push_back(next_block);
                block_id = next_block;
                pos = 0;
            }
        } else {
            // Direct copy — port of compression.rs:184-188
            output.push_back(byte);
        }
    }

    // Pad to kSongSize if needed — port of compression.rs:191-193
    if (output.size() < kSongSize) {
        output.resize(kSongSize, 0);
    }

    return output;
}

}  // namespace rackboy::sav::compression
