// tests/sav/header_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_header.{h,cpp} — the
// container-layer .sav validator for rackboy-hg5.
//
// The tests build a synthetic 128 KiB buffer in memory, populate
// only the bytes the header validator actually reads, and flip one
// field at a time to confirm each failure code. The committed
// fixture at `tests/fixtures/rackboy_v1.sav` is also loaded when
// available so the tests double as an integration smoke against a
// real LSDJ 9.4.0 sav.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "doctest/doctest.h"

#include "rackboy_sav_header.h"

using rackboy::sav::HeaderError;
using rackboy::sav::HeaderInfo;
using rackboy::sav::header_error_string;
using rackboy::sav::kActiveSlotOffset;
using rackboy::sav::kHeaderVersionLsdj940;
using rackboy::sav::kHeaderVersionOffset;
using rackboy::sav::kLsdjMaxSlotExclusive;
using rackboy::sav::kMagicByte0;
using rackboy::sav::kMagicByte1;
using rackboy::sav::kMagicOffset;
using rackboy::sav::kRackboyV1ActiveSlot;
using rackboy::sav::kSavSizeBytes;
using rackboy::sav::read_header;
using rackboy::sav::validate_header;

namespace {

// Build a 128 KiB zero-filled buffer and stamp the exact bytes the
// header validator reads with valid LSDJ 9.4.0 values. The rest of
// the buffer is left as 0x00 on purpose — the validator does not
// care about bytes outside the handful of offsets in the header,
// and neither should the tests. Returns by value; callers are
// expected to rely on RVO / std::move rather than a heap alloc
// behind a smart pointer.
std::vector<std::uint8_t> make_valid_buffer()
{
    std::vector<std::uint8_t> buf(kSavSizeBytes, 0);
    buf[kMagicOffset + 0]       = kMagicByte0;
    buf[kMagicOffset + 1]       = kMagicByte1;
    buf[kActiveSlotOffset]      = kRackboyV1ActiveSlot;
    buf[kHeaderVersionOffset]   = kHeaderVersionLsdj940;
    return buf;
}

// Try to load the committed rackboy_v1 fixture. Returns an empty
// vector on failure (test runs skip that case gracefully — the
// unit tests above cover every error path on a synthetic buffer so
// the fixture is an extra-cost smoke check, not a dependency).
std::vector<std::uint8_t> try_load_fixture()
{
    // The test binaries run from the repo root via `make -C tests
    // test` (see tests/Makefile), so the fixture path is
    // "fixtures/rackboy_v1.sav" relative to tests/. We try a few
    // candidate paths so the test is also discoverable if run from
    // tests/build/sav/ or from the repo root directly.
    const char* candidates[] = {
        "fixtures/rackboy_v1.sav",
        "tests/fixtures/rackboy_v1.sav",
        "../fixtures/rackboy_v1.sav",
        "../../fixtures/rackboy_v1.sav",
    };
    for (const char* path : candidates) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::vector<std::uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        if (!bytes.empty()) {
            return bytes;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("validate_header rejects nullptr")
{
    CHECK_EQ(validate_header(nullptr, kSavSizeBytes),
             HeaderError::NullBuffer);
    CHECK_EQ(validate_header(nullptr, 0),
             HeaderError::NullBuffer);
}

TEST_CASE("validate_header rejects wrong size buffers")
{
    // Zero length.
    std::uint8_t byte = 0;
    CHECK_EQ(validate_header(&byte, 0), HeaderError::SizeMismatch);

    // Off-by-one short.
    std::vector<std::uint8_t> too_short(kSavSizeBytes - 1, 0);
    CHECK_EQ(validate_header(too_short.data(), too_short.size()),
             HeaderError::SizeMismatch);

    // Off-by-one long — even adding a single byte is rejected.
    std::vector<std::uint8_t> too_long(kSavSizeBytes + 1, 0);
    CHECK_EQ(validate_header(too_long.data(), too_long.size()),
             HeaderError::SizeMismatch);
}

TEST_CASE("validate_header rejects missing 'jk' magic")
{
    // Zero buffer (magic bytes are 0x00 0x00 — not 'jk').
    std::vector<std::uint8_t> buf(kSavSizeBytes, 0);
    CHECK_EQ(validate_header(buf.data(), buf.size()),
             HeaderError::MagicMismatch);

    // Correct magic then corrupt only one of the two bytes.
    SUBCASE("first byte wrong")
    {
        auto b = make_valid_buffer();
        b[kMagicOffset + 0] = 0x00;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::MagicMismatch);
    }
    SUBCASE("second byte wrong")
    {
        auto b = make_valid_buffer();
        b[kMagicOffset + 1] = 0x00;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::MagicMismatch);
    }
}

TEST_CASE("validate_header rejects active-slot out of LSDJ 0..31 range")
{
    // The validator checks size and magic before active-slot range,
    // so start from a fully-valid buffer and only flip the slot.
    auto b = make_valid_buffer();

    SUBCASE("exactly at the boundary (32)")
    {
        b[kActiveSlotOffset] = kLsdjMaxSlotExclusive;  // == 32
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::ActiveSlotOutOfRange);
    }
    SUBCASE("max uint8_t")
    {
        b[kActiveSlotOffset] = 0xFF;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::ActiveSlotOutOfRange);
    }
    SUBCASE("just past the boundary (33)")
    {
        b[kActiveSlotOffset] = kLsdjMaxSlotExclusive + 1;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::ActiveSlotOutOfRange);
    }
}

TEST_CASE("validate_header rejects active slot != 0 (v1 restriction)")
{
    auto b = make_valid_buffer();

    // Any in-range non-zero slot should report ActiveSlotNotZero,
    // not ActiveSlotOutOfRange.
    SUBCASE("slot 1")
    {
        b[kActiveSlotOffset] = 1;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::ActiveSlotNotZero);
    }
    SUBCASE("slot 31 (last valid LSDJ slot)")
    {
        b[kActiveSlotOffset] = 31;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::ActiveSlotNotZero);
    }
}

TEST_CASE("validate_header rejects wrong LSDJ header version byte")
{
    auto b = make_valid_buffer();

    SUBCASE("0x00 (empty-template stub value)")
    {
        b[kHeaderVersionOffset] = 0x00;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::VersionMismatch);
    }
    SUBCASE("one below LSDJ 9.4.0")
    {
        b[kHeaderVersionOffset] =
            static_cast<std::uint8_t>(kHeaderVersionLsdj940 - 1);
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::VersionMismatch);
    }
    SUBCASE("one above LSDJ 9.4.0")
    {
        b[kHeaderVersionOffset] =
            static_cast<std::uint8_t>(kHeaderVersionLsdj940 + 1);
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::VersionMismatch);
    }
    SUBCASE("0xFF")
    {
        b[kHeaderVersionOffset] = 0xFF;
        CHECK_EQ(validate_header(b.data(), b.size()),
                 HeaderError::VersionMismatch);
    }
}

TEST_CASE("validate_header accepts a synthetic valid buffer")
{
    auto b = make_valid_buffer();
    CHECK_EQ(validate_header(b.data(), b.size()), HeaderError::Ok);
}

TEST_CASE("read_header populates HeaderInfo on success")
{
    auto b = make_valid_buffer();
    HeaderInfo info{};
    // Poison the out-param to prove read_header actually wrote it.
    info.active_slot    = 0xEE;
    info.header_version = 0xEE;
    info.magic_ok       = false;

    CHECK_EQ(read_header(b.data(), b.size(), &info), HeaderError::Ok);
    CHECK_EQ(info.active_slot,    kRackboyV1ActiveSlot);
    CHECK_EQ(info.header_version, kHeaderVersionLsdj940);
    CHECK(info.magic_ok);
}

TEST_CASE("read_header leaves HeaderInfo untouched on every non-Ok path")
{
    // Every non-Ok path must leave `*out` alone so callers can rely
    // on "pre-set the struct, call read_header, only trust fields
    // on Ok" without having to branch on the return code inside the
    // else arm. The implementation happens to honor this by
    // construction (the single write to *out is at the very end,
    // after every check passes), but a future refactor could
    // regress the contract by writing a field before the matching
    // validation step — this sweep locks every code path so the
    // suite fails loudly instead of silently in that case.
    //
    // `NullBuffer` is exempt: it takes nullptr in, there is no
    // buffer to mutate, and there is no meaningful "out param
    // untouched" story for a call that cannot have touched
    // anything anyway.

    auto poison_info = []() {
        HeaderInfo info{};
        info.active_slot    = 0xEE;
        info.header_version = 0xEE;
        info.magic_ok       = true;  // wrong sentinel on purpose
        return info;
    };

    auto check_untouched = [](const HeaderInfo& info) {
        CHECK_EQ(info.active_slot,    std::uint8_t{0xEE});
        CHECK_EQ(info.header_version, std::uint8_t{0xEE});
        CHECK(info.magic_ok);
    };

    SUBCASE("SizeMismatch")
    {
        // Short buffer never reaches the magic / slot / version
        // reads. Still must leave the info struct untouched.
        std::vector<std::uint8_t> short_buf(kSavSizeBytes - 1, 0);
        HeaderInfo info = poison_info();
        CHECK_EQ(read_header(short_buf.data(), short_buf.size(), &info),
                 HeaderError::SizeMismatch);
        check_untouched(info);
    }

    SUBCASE("MagicMismatch")
    {
        // Zero buffer passes the size check but fails at magic.
        std::vector<std::uint8_t> b(kSavSizeBytes, 0);
        HeaderInfo info = poison_info();
        CHECK_EQ(read_header(b.data(), b.size(), &info),
                 HeaderError::MagicMismatch);
        check_untouched(info);
    }

    SUBCASE("ActiveSlotOutOfRange")
    {
        auto b = make_valid_buffer();
        b[kActiveSlotOffset] = kLsdjMaxSlotExclusive;  // == 32
        HeaderInfo info = poison_info();
        CHECK_EQ(read_header(b.data(), b.size(), &info),
                 HeaderError::ActiveSlotOutOfRange);
        check_untouched(info);
    }

    SUBCASE("ActiveSlotNotZero")
    {
        auto b = make_valid_buffer();
        b[kActiveSlotOffset] = 1;
        HeaderInfo info = poison_info();
        CHECK_EQ(read_header(b.data(), b.size(), &info),
                 HeaderError::ActiveSlotNotZero);
        check_untouched(info);
    }

    SUBCASE("VersionMismatch")
    {
        auto b = make_valid_buffer();
        b[kHeaderVersionOffset] = 0x00;
        HeaderInfo info = poison_info();
        CHECK_EQ(read_header(b.data(), b.size(), &info),
                 HeaderError::VersionMismatch);
        check_untouched(info);
    }
}

TEST_CASE("read_header accepts a null out-pointer (equivalent to validate_header)")
{
    auto b = make_valid_buffer();
    CHECK_EQ(read_header(b.data(), b.size(), nullptr), HeaderError::Ok);
}

TEST_CASE("header_error_string returns a non-null, distinct, non-empty "
          "string for every error")
{
    // The switch in rackboy_sav_header.cpp deliberately avoids a
    // default case so a future enumerator addition is caught by
    // -Wswitch. This test additionally asserts every currently-
    // declared value maps to a *distinct* non-empty string, so a
    // copy/paste bug in the switch (two enumerators returning the
    // same literal) cannot slip past review.
    const HeaderError all[] = {
        HeaderError::Ok,
        HeaderError::NullBuffer,
        HeaderError::SizeMismatch,
        HeaderError::MagicMismatch,
        HeaderError::ActiveSlotOutOfRange,
        HeaderError::ActiveSlotNotZero,
        HeaderError::VersionMismatch,
    };
    constexpr std::size_t kAllCount = sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (HeaderError e : all) {
        const char* s = header_error_string(e);
        REQUIRE(s != nullptr);
        CHECK(s[0] != '\0');
        // Compare against every previously-seen string; a duplicate
        // is a bug.
        for (std::size_t i = 0; i < seen_count; ++i) {
            CAPTURE(i);
            CAPTURE(s);
            CAPTURE(seen[i]);
            CHECK(std::strcmp(seen[i], s) != 0);
        }
        seen[seen_count++] = s;
    }
    CHECK_EQ(seen_count, kAllCount);
}

TEST_CASE("validate_header accepts the committed rackboy_v1.sav fixture")
{
    // Integration smoke: if we can find the fixture the
    // spike/expected.json is pinned to, the production validator
    // must agree with the spike's container-level checks.
    auto bytes = try_load_fixture();
    if (bytes.empty()) {
        // Fixture not reachable from this run's cwd — skip.
        // Synthetic-buffer tests above already cover every code
        // path this fixture exercises.
        MESSAGE("rackboy_v1.sav fixture not found; skipping smoke case");
        return;
    }
    REQUIRE(bytes.size() == kSavSizeBytes);

    HeaderInfo info{};
    CHECK_EQ(read_header(bytes.data(), bytes.size(), &info),
             HeaderError::Ok);
    CHECK_EQ(info.active_slot, kRackboyV1ActiveSlot);
    CHECK_EQ(info.header_version, kHeaderVersionLsdj940);
    CHECK(info.magic_ok);
}
