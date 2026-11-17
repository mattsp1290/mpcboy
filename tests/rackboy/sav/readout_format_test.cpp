// tests/sav/readout_format_test.cpp
//
// Pure-logic tests for the `format_readout_text` helper used by
// the VCV widget (`src/RackboyWidget.cpp`) and, eventually, the
// Daisy OLED readout (bead `rackboy-1i2`). The helper has no
// dependency on NanoVG or the Rack SDK, so it can be exercised
// directly from the doctest host harness without any additional
// build plumbing.
//
// Invariant coverage: the test cases below lock down every branch
// of the helper — out-of-range channel, no-instrument sentinel,
// empty-name fallback to slot number, populated-name path, and
// the tiny-buffer / null-buffer safety clauses. Matches the
// "prefer pure helpers that can be tested without the surrounding
// framework" guidance in the 2026-04-18 iteration-64 code review.

#include <cstdint>
#include <cstring>
#include <string>

#include "doctest/doctest.h"

#include "rackboy_readout_format.h"

namespace {

// Small helper so each TEST_CASE can assert "this formatted buffer
// matches this exact C string". doctest's default `==` on two
// `const char*` pointers does pointer-identity; comparing the
// contents through `std::string` avoids that footgun without
// dragging in `CHECK_EQ(std::strcmp(...) == 0)` boilerplate.
inline std::string as_string(const char* c)
{
    return std::string(c);
}

}  // namespace

TEST_CASE("format_readout_text: channel prefixes") {
    char buf[24];

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, 0, nullptr);
    CHECK(as_string(buf) == "PU1: 00");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 1, 0, nullptr);
    CHECK(as_string(buf) == "PU2: 00");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 2, 0, nullptr);
    CHECK(as_string(buf) == "WV: 00");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 3, 0, nullptr);
    CHECK(as_string(buf) == "NO: 00");
}

TEST_CASE("format_readout_text: slot number fallback is zero-padded") {
    char buf[24];

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, 1, nullptr);
    CHECK(as_string(buf) == "PU1: 01");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, 9, nullptr);
    CHECK(as_string(buf) == "PU1: 09");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, 63, nullptr);
    CHECK(as_string(buf) == "PU1: 63");
}

TEST_CASE("format_readout_text: populated name takes precedence over slot") {
    char buf[24];

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, 3, "LEAD");
    CHECK(as_string(buf) == "PU1: LEAD");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 2, 0, "PAD");
    CHECK(as_string(buf) == "WV: PAD");
}

TEST_CASE("format_readout_text: empty-string name falls back to slot") {
    char buf[24];

    // `name[0] == '\0'` is the "no sidecar entry for this slot"
    // shape produced by `names_reset` (rackboy_sav_sidecar.h).
    rackboy::sav::format_readout_text(buf, sizeof(buf), 1, 5, "");
    CHECK(as_string(buf) == "PU2: 05");
}

TEST_CASE("format_readout_text: negative idx renders as no-instrument") {
    char buf[24];

    // kNoInstrument == -1, but the helper's contract is "any
    // negative idx" so regression-guard both -1 and other sentinels.
    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, -1, nullptr);
    CHECK(as_string(buf) == "PU1: --");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 0, -5,
                                      "UNUSED-NAME");
    CHECK(as_string(buf) == "PU1: --");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 3, -1, nullptr);
    CHECK(as_string(buf) == "NO: --");
}

TEST_CASE("format_readout_text: out-of-range channel shows ??") {
    char buf[24];

    rackboy::sav::format_readout_text(buf, sizeof(buf), -1, 0, nullptr);
    CHECK(as_string(buf) == "??: --");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 4, 0, nullptr);
    CHECK(as_string(buf) == "??: --");

    rackboy::sav::format_readout_text(buf, sizeof(buf), 99, 3, "LEAD");
    CHECK(as_string(buf) == "??: --");
}

TEST_CASE("format_readout_text: bounded writes (tiny buffers)") {
    // n == 0 must not touch `buf`. Poison the buffer first; the
    // helper should leave the poison bytes untouched.
    char buf[8] = {'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X'};
    rackboy::sav::format_readout_text(buf, 0, 0, 3, "LEAD");
    CHECK(buf[0] == 'X');
    CHECK(buf[7] == 'X');

    // n == 1 is "just the null terminator" — snprintf guarantees
    // the buffer is null-terminated even when truncation happens.
    char tiny[1] = {'\xff'};
    rackboy::sav::format_readout_text(tiny, sizeof(tiny), 0, 0,
                                      "LEAD");
    CHECK(tiny[0] == '\0');

    // A small-but-nonzero buffer should truncate safely and
    // null-terminate.
    char small[5] = {0};
    rackboy::sav::format_readout_text(small, sizeof(small), 0, 0,
                                      "LEAD");
    // "PU1: " is exactly 5 bytes plus NUL; a 5-byte buffer holds
    // 4 payload bytes plus NUL. Whatever ends up in the buffer
    // must at minimum be null-terminated at `small[4]`.
    CHECK(small[sizeof(small) - 1] == '\0');
}

TEST_CASE("format_readout_text: null buffer pointer is a no-op") {
    // The contract allows `buf == nullptr` to safely return without
    // writing anywhere. Without this guard a buggy caller would
    // segfault inside snprintf.
    rackboy::sav::format_readout_text(nullptr, 24, 0, 0, "LEAD");
    // Reaching this line is the assertion.
    CHECK(true);
}

TEST_CASE("readout_channel_prefix: valid + out-of-range indices") {
    CHECK(as_string(rackboy::sav::readout_channel_prefix(0)) == "PU1");
    CHECK(as_string(rackboy::sav::readout_channel_prefix(1)) == "PU2");
    CHECK(as_string(rackboy::sav::readout_channel_prefix(2)) == "WV");
    CHECK(as_string(rackboy::sav::readout_channel_prefix(3)) == "NO");

    CHECK(rackboy::sav::readout_channel_prefix(-1) == nullptr);
    CHECK(rackboy::sav::readout_channel_prefix(4)  == nullptr);
    CHECK(rackboy::sav::readout_channel_prefix(99) == nullptr);
}
