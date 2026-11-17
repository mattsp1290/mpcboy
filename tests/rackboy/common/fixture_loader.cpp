// tests/common/fixture_loader.cpp
//
// Implementation of the shared fixture loader declared in
// fixture_loader.h (bead rackboy-8jy).
//
// All three helpers were previously duplicated in
// tests/sav/fixture_test.cpp and
// tests/table/fixture_integration_test.cpp. Moving them here keeps
// the single canonical fixture / single canonical hash invariant
// true as more fixture-backed suites land.

#include "fixture_loader.h"

#include "doctest/doctest.h"

#include <cstdio>

namespace rackboy {
namespace tests {
namespace common {

std::vector<std::uint8_t> load_fixture_bytes()
{
    const char* candidates[] = {
        "fixtures/rackboy_v1.sav",       // from tests/
        "tests/fixtures/rackboy_v1.sav", // from repo root
        "../fixtures/rackboy_v1.sav",    // from tests/build/
        "../../fixtures/rackboy_v1.sav", // from tests/build/{sav,table}/
    };
    for (const char* path : candidates) {
        std::FILE* fp = std::fopen(path, "rb");
        if (fp == nullptr) {
            continue;
        }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        REQUIRE(size > 0);

        // Hard gate: wrong-sized file fails loudly here rather than
        // proceeding to fixed-offset reads downstream. CAPTURE the
        // path so a multi-path CI host can tell which candidate
        // was the wrong size.
        CAPTURE(path);
        REQUIRE_EQ(static_cast<std::size_t>(size), kFixtureSize);

        std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
        const std::size_t read =
            std::fread(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
        REQUIRE_EQ(read, buf.size());
        return buf;
    }
    FAIL("rackboy_v1.sav fixture not found — tried fixtures/, "
         "tests/fixtures/, ../fixtures/, ../../fixtures/ "
         "relative to cwd");
    return {};  // unreachable
}

std::uint64_t fnv1a_64(const std::vector<std::uint8_t>& bytes)
{
    constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime       = 0x100000001b3ULL;
    std::uint64_t h = kOffsetBasis;
    for (std::uint8_t b : bytes) {
        h ^= static_cast<std::uint64_t>(b);
        h *= kPrime;
    }
    return h;
}

std::uint8_t find_first_block_for_slot(
    const std::vector<std::uint8_t>& sav, std::uint8_t slot)
{
    // Offsets pinned by the LSDJ .sav layout; duplicated from both
    // previous copies of this helper. If either the start offset or
    // the length ever needs to change, update here only.
    constexpr std::size_t kBlockAllocOffset = 0x8141;
    constexpr std::size_t kBlockAllocLen    = 0xBF;  // 191
    for (std::size_t i = 0; i < kBlockAllocLen; ++i) {
        if (sav[kBlockAllocOffset + i] == slot) {
            return static_cast<std::uint8_t>(i);
        }
    }
    FAIL("block allocation table had no entry owned by slot " << int{slot});
    return 0xFF;  // unreachable
}

}  // namespace common
}  // namespace tests
}  // namespace rackboy
