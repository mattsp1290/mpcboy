// src/plugin/state_codec.h - the extra-state blob (WP7, version 2 since WP11).
// No SDK dependency. Little-endian:
//   0     4     magic "MBST"
//   4     1     format version (2)
//   5     1792  16 tables x 16 rows x 7 bytes: has_vol, vol, transpose (int8), cmd1, val1, cmd2, val2
//   1797  256   16 waves x 16 bytes
//   2053  1     length N of the last imported file name (0..255)
//   2054  N     the file name, UTF-8 bytes
//   2054+N 4    CRC-32 (IEEE) of every byte before it
// Total 2058 + N bytes. Version 1 (no name field, 2057 bytes) is no longer
// accepted: no users, no compatibility requirement (the freeze rule starts at
// the first tagged release).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "table_bank.h"

namespace mpcboy {

constexpr size_t kStateSize = 2058;        // with an empty file name
constexpr size_t kStateMaxName = 255;
constexpr size_t kStateMaxSize = kStateSize + kStateMaxName;
constexpr uint8_t kStateVersion = 2;

uint32_t crc32Ieee(const uint8_t* data, size_t size);

// Appends kStateSize + min(name.size(), 255) bytes to `out`. A longer name is
// cut at 255 bytes.
void encodeState(const Content& c, std::vector<uint8_t>& out, const std::string& importName = std::string());

// Validates magic, version, size and CRC, then fills `out` (every table is
// marked occupied; command bytes are stored unchanged) and, when non-null,
// `importName`. Returns false and leaves both untouched on any error. Never
// reads past `size`.
bool decodeState(const uint8_t* data, size_t size, Content& out, std::string* importName = nullptr);

}  // namespace mpcboy
