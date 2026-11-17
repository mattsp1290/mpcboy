// src/plugin/state_codec.h - the extra-state blob (WP7), format version 1.
// No SDK dependency. Little-endian:
//   0     4     magic "MBST"
//   4     1     format version (1)
//   5     1792  16 tables x 16 rows x 7 bytes: has_vol, vol, transpose (int8), cmd1, val1, cmd2, val2
//   1797  256   16 waves x 16 bytes
//   2053  4     CRC-32 (IEEE) of bytes 0..2052
// Total 2057 bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "table_bank.h"

namespace mpcboy {

constexpr size_t kStateSize = 2057;
constexpr uint8_t kStateVersion = 1;

uint32_t crc32Ieee(const uint8_t* data, size_t size);

// Appends kStateSize bytes to `out`.
void encodeState(const Content& c, std::vector<uint8_t>& out);

// Validates magic, version, size and CRC, then fills `out` (every table is
// marked occupied; command bytes are stored unchanged). Returns false and
// leaves `out` untouched on any error. Never reads past `size`.
bool decodeState(const uint8_t* data, size_t size, Content& out);

}  // namespace mpcboy
