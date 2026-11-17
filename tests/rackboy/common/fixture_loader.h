// tests/common/fixture_loader.h
//
// Shared fixture-loading helpers for test binaries that consume
// `tests/fixtures/rackboy_v1.sav`. Bead rackboy-8jy.
//
// Motivation
// ----------
// Two test binaries need to load the same 128 KiB LSDJ `.sav` and
// walk it through the shared parser pipeline:
//
//   * `tests/sav/fixture_test.cpp` — end-to-end sav parser coverage
//   * `tests/table/fixture_integration_test.cpp` — table interpreter
//     integration tests that need a populated bank
//
// Before this extraction, each file carried its own copy of the
// loader, the FNV-1a drift detector, and the block-alloc walker —
// already starting to diverge (different FAIL messages, different
// constant scoping). Centralizing here keeps the "single canonical
// fixture + one authoritative hash" invariant intact as more
// integration tests land (golden-audio regression under
// rackboy-5wl, etc.).
//
// Scope
// -----
// This header is TEST-ONLY. It uses `doctest`'s REQUIRE / FAIL
// macros directly (via the implementation file) and is therefore
// only safe to include from a translation unit that itself
// includes `doctest/doctest.h` — i.e. any tests/**/*_test.cpp
// file. It MUST NOT be linked into any production artifact.
//
// The loader knows NOTHING about the sav layout beyond the bare
// minimum needed to find slot 0's first block; fixture validation
// is the sav fixture test's job. Callers that need a populated
// `RackboyBank` run the returned bytes through
// `rackboy::sav::load_rackboy_sav` themselves — there's a helper
// for that on the table side because it needs the parser + the
// spike decompressor linked in. The sav-side binary already links
// both, so its fixture test can use the same pipeline directly.

#ifndef RACKBOY_TESTS_COMMON_FIXTURE_LOADER_H_
#define RACKBOY_TESTS_COMMON_FIXTURE_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rackboy {
namespace tests {
namespace common {

// Expected size of the committed LSDJ fixture
// (`tests/fixtures/rackboy_v1.sav`). A hard gate: any file found at
// one of the candidate paths that does not match this size fails the
// REQUIRE inside `load_fixture_bytes` rather than proceeding to
// downstream fixed-offset reads that would silently produce OOB.
constexpr std::size_t kFixtureSize = 131072;

// FNV-1a 64-bit fingerprint of the committed fixture. Pairs with
// `fnv1a_64(load_fixture_bytes())` for the drift-detector
// TEST_CASEs in both the sav and the table integration suites.
//
// When the fixture is legitimately regenerated, update THIS constant
// plus the per-channel count / offset expectations in each fixture
// test — all in the same commit so drift is obvious in the diff.
constexpr std::uint64_t kExpectedFixtureFnv1a64 =
    0x00b41cf802deb490ULL;

// Load the committed LSDJ fixture from whichever cwd the test
// binary was launched from. Fails the current doctest test case
// (via `FAIL`) if the file cannot be opened on any candidate path
// OR is the wrong size.
//
// Candidate search order matches the convention previously
// duplicated in tests/sav/fixture_test.cpp and
// tests/table/fixture_integration_test.cpp:
//
//   `fixtures/rackboy_v1.sav`          — from tests/
//   `tests/fixtures/rackboy_v1.sav`    — from repo root
//   `../fixtures/rackboy_v1.sav`       — from tests/build/
//   `../../fixtures/rackboy_v1.sav`    — from tests/build/sav/ or
//                                         tests/build/table/
//
// Callers should NOT depend on which path succeeded; the returned
// buffer is the same regardless.
std::vector<std::uint8_t> load_fixture_bytes();

// FNV-1a 64-bit over an arbitrary byte buffer. Not cryptographically
// secure; used strictly as a deterministic fingerprint. Constants
// from https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function.
std::uint64_t fnv1a_64(const std::vector<std::uint8_t>& bytes);

// Walk the 191-byte block-allocation table at 0x8141 and return the
// block index of the first block owned by `slot`. The "first match"
// convention is correct for the LSDJ canonical layout (chain-start
// block is lowest-indexed); the committed fixture follows it. Fails
// the current test case if no block owned by `slot` is found.
std::uint8_t find_first_block_for_slot(
    const std::vector<std::uint8_t>& sav, std::uint8_t slot);

}  // namespace common
}  // namespace tests
}  // namespace rackboy

#endif  // RACKBOY_TESTS_COMMON_FIXTURE_LOADER_H_
