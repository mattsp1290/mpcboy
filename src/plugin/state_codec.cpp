#include "state_codec.h"

#include <cstring>

namespace mpcboy {

namespace {
constexpr size_t kTablesOffset = 5;
constexpr size_t kRowBytes = 7;
constexpr size_t kWavesOffset = kTablesOffset + kNumTables * 16 * kRowBytes;   // 1797
constexpr size_t kCrcOffset = kWavesOffset + kNumWaves * kWaveBytes;           // 2053
static_assert(kWavesOffset == 1797 && kCrcOffset == 2053 && kCrcOffset + 4 == kStateSize, "state layout");
}  // namespace

uint32_t crc32Ieee(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void encodeState(const Content& c, std::vector<uint8_t>& out)
{
    const size_t base = out.size();
    out.resize(base + kStateSize);
    uint8_t* p = out.data() + base;
    std::memcpy(p, "MBST", 4);
    p[4] = kStateVersion;
    uint8_t* q = p + kTablesOffset;
    for (int t = 0; t < kNumTables; ++t)
        for (int r = 0; r < 16; ++r)
        {
            const auto& row = c.tables.tables[t].rows[r];
            q[0] = row.has_vol ? 1 : 0;
            q[1] = row.vol;
            q[2] = static_cast<uint8_t>(row.transpose);
            q[3] = row.cmd1;
            q[4] = row.cmd1_param;
            q[5] = row.cmd2;
            q[6] = row.cmd2_param;
            q += kRowBytes;
        }
    std::memcpy(p + kWavesOffset, c.waves.waves, sizeof(c.waves.waves));
    const uint32_t crc = crc32Ieee(p, kCrcOffset);
    for (int i = 0; i < 4; ++i) p[kCrcOffset + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
}

bool decodeState(const uint8_t* data, size_t size, Content& out)
{
    if (data == nullptr || size != kStateSize) return false;
    if (std::memcmp(data, "MBST", 4) != 0 || data[4] != kStateVersion) return false;
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored |= static_cast<uint32_t>(data[kCrcOffset + static_cast<size_t>(i)]) << (8 * i);
    if (stored != crc32Ieee(data, kCrcOffset)) return false;

    Content c;
    clearContent(c);
    const uint8_t* q = data + kTablesOffset;
    for (int t = 0; t < kNumTables; ++t)
    {
        c.tables.tables[t].occupied = true;
        for (int r = 0; r < 16; ++r)
        {
            auto& row = c.tables.tables[t].rows[r];
            row.has_vol = q[0] != 0;
            row.vol = q[1];
            row.transpose = static_cast<int8_t>(q[2]);
            row.cmd1 = q[3];
            row.cmd1_param = q[4];
            row.cmd2 = q[5];
            row.cmd2_param = q[6];
            q += kRowBytes;
        }
    }
    std::memcpy(c.waves.waves, data + kWavesOffset, sizeof(c.waves.waves));
    out = c;
    return true;
}

}  // namespace mpcboy
