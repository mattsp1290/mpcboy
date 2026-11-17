// tests/sav/sidecar_test.cpp
//
// Unit tests for rackboy_sav/rackboy_sav_sidecar.{h,cpp} —
// the optional `<savname>.json` name-sidecar parser for
// bead rackboy-69j.
//
// The sidecar JSON subset is deliberately strict (see the
// header for the schema). These tests lock every soft-fail
// error code, the happy-path parse, the poison-and-check
// soft-fail invariant, and the boundary conditions of the
// `PU1-NN` / `PU2-NN` / `WV-NN` / `NO-NN` key format.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "doctest/doctest.h"

#include "rackboy_sav_sidecar.h"
#include "rackboy_sav_types.h"  // kMaxInstrumentsPerChannel

using rackboy::sav::kMaxInstrumentNameBytes;
using rackboy::sav::kMaxInstrumentsPerChannel;
using rackboy::sav::names_reset;
using rackboy::sav::parse_sidecar;
using rackboy::sav::RackboyBankNames;
using rackboy::sav::RackboyInstrumentName;
using rackboy::sav::sidecar_error_string;
using rackboy::sav::SidecarError;

namespace {

// Call parse_sidecar with a std::string body so the test
// source stays readable. The parser itself takes a raw
// pointer + length so non-null-terminated buffers work, but
// tests use strings for clarity.
SidecarError parse(const std::string& json,
                   RackboyBankNames&  out)
{
    return parse_sidecar(json.data(), json.size(), &out);
}

// Check that a given slot's stored name equals an expected
// C string. Uses strcmp rather than a byte loop because the
// parser guarantees null termination.
void check_name(const RackboyInstrumentName& slot,
                const char*                  expected)
{
    CHECK_EQ(std::strcmp(slot.text, expected), 0);
}

// Byte-exact variant: checks that the slot matches
// `expected` up to the null terminator AND that every byte
// past the terminator is zero. Used by tests that want to
// prove the POD layout stays byte-for-byte deterministic
// across duplicate-key overwrites — `check_name`'s strcmp
// cannot see past the first `\0`, so stale tail bytes
// would be invisible.
void check_name_exact(const RackboyInstrumentName& slot,
                      const char*                  expected)
{
    const std::size_t elen = std::strlen(expected);
    REQUIRE(elen < kMaxInstrumentNameBytes);
    for (std::size_t i = 0; i < elen; ++i) {
        CAPTURE(i);
        CHECK_EQ(slot.text[i], expected[i]);
    }
    for (std::size_t i = elen; i < kMaxInstrumentNameBytes; ++i) {
        CAPTURE(i);
        CHECK_EQ(slot.text[i], '\0');
    }
}

// Every byte of every slot must be zero.
void check_all_names_empty(const RackboyBankNames& n)
{
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        CAPTURE(i);
        CHECK_EQ(n.pulse1[i].text[0], '\0');
        CHECK_EQ(n.pulse2[i].text[0], '\0');
        CHECK_EQ(n.wave  [i].text[0], '\0');
        CHECK_EQ(n.noise [i].text[0], '\0');
        for (std::size_t b = 1; b < kMaxInstrumentNameBytes; ++b) {
            CAPTURE(b);
            CHECK_EQ(n.pulse1[i].text[b], '\0');
            CHECK_EQ(n.pulse2[i].text[b], '\0');
            CHECK_EQ(n.wave  [i].text[b], '\0');
            CHECK_EQ(n.noise [i].text[b], '\0');
        }
    }
}

// Poison every slot with a recognizable byte pattern so a
// subsequent test can prove names_reset ran over the whole
// struct on the error path.
void poison_names(RackboyBankNames& n)
{
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        for (std::size_t b = 0; b < kMaxInstrumentNameBytes; ++b) {
            n.pulse1[i].text[b] = 'A';
            n.pulse2[i].text[b] = 'B';
            n.wave  [i].text[b] = 'C';
            n.noise [i].text[b] = 'D';
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Top-level structural errors
// ---------------------------------------------------------------------------

TEST_CASE("parse_sidecar rejects null out_names")
{
    const std::string json = "{}";
    CHECK_EQ(parse_sidecar(json.data(), json.size(), nullptr),
             SidecarError::NullOutNames);
}

TEST_CASE("parse_sidecar rejects null json buffer")
{
    RackboyBankNames names{};
    CHECK_EQ(parse_sidecar(nullptr, 16, &names),
             SidecarError::NullJson);
}

TEST_CASE("parse_sidecar NullJson leaves names[] cleared even when "
          "caller poisoned the struct")
{
    RackboyBankNames names;
    poison_names(names);

    CHECK_EQ(parse_sidecar(nullptr, 16, &names),
             SidecarError::NullJson);
    check_all_names_empty(names);
}

TEST_CASE("parse_sidecar accepts an empty object")
{
    RackboyBankNames names;
    poison_names(names);

    const std::string json = "{}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_all_names_empty(names);
}

TEST_CASE("parse_sidecar tolerates surrounding whitespace")
{
    RackboyBankNames names{};
    const std::string json = "   \n\t  {  }  \r\n ";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_all_names_empty(names);
}

TEST_CASE("parse_sidecar rejects an empty buffer")
{
    RackboyBankNames names{};
    CHECK_EQ(parse(std::string{}, names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects whitespace-only buffer")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("   \n\t   ", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects missing closing brace")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects garbage after closing brace")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{}x", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects top-level array")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("[]", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects non-string key")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{1: \"X\"}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects non-string value")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\": 1}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects missing colon")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\" \"X\"}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects trailing comma")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\": \"X\",}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects missing comma between entries")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\": \"X\" \"PU1-01\": \"Y\"}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects unterminated string")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects any backslash escape in a string")
{
    RackboyBankNames names{};
    // Even a valid-looking \" escape is rejected by the
    // strict subset — sidecar bodies are ASCII printable
    // with no escapes.
    CHECK_EQ(parse("{\"PU1-00\": \"A\\\"B\"}", names),
             SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects control bytes inside a string")
{
    RackboyBankNames names{};
    std::string json = "{\"PU1-00\": \"A\x01\"}";  // SOH inside value
    CHECK_EQ(parse(json, names), SidecarError::InvalidJson);
}

// ---------------------------------------------------------------------------
// Key classification errors
// ---------------------------------------------------------------------------

TEST_CASE("parse_sidecar rejects lower-case channel prefix")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"pu1-00\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects unknown channel prefix")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"XX-00\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects empty key")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects high-bit-set bytes inside strings")
{
    RackboyBankNames names{};
    // 0xC2 0xA9 is UTF-8 for U+00A9 (copyright sign). The
    // strict subset is ASCII-printable only, so any byte
    // with the high bit set is InvalidJson.
    const std::string json = "{\"PU1-00\": \"\xC2\xA9\"}";
    CHECK_EQ(parse(json, names), SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects DEL (0x7F) inside strings")
{
    RackboyBankNames names{};
    // DEL sits above the printable ASCII range (0x20..0x7E)
    // and is rejected alongside every other control byte.
    // Split the literal so the `B` is not pulled into the
    // hex escape: C++ would otherwise treat `\x7FB` as a
    // three-hex-digit escape that overflows char.
    const std::string json = "{\"PU1-00\": \"A\x7F" "B\"}";
    CHECK_EQ(parse(json, names), SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects a non-printable byte inside a key")
{
    // parse_string is shared by keys and values, so in
    // principle the value-side control-byte tests above
    // already cover the key side. Lock the coverage
    // explicitly so a future refactor that splits the key
    // and value string paths cannot accidentally narrow
    // the contract.
    RackboyBankNames names{};
    std::string json = "{\"PU1-";
    json.push_back(static_cast<char>(0x7F));
    json += "0\": \"X\"}";
    CHECK_EQ(parse(json, names), SidecarError::InvalidJson);
}

TEST_CASE("parse_sidecar rejects one-digit slot")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-3\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects missing dash")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU100\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects non-digit slot number")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-0A\": \"X\"}", names),
             SidecarError::UnknownChannel);
}

TEST_CASE("parse_sidecar rejects slot >= kMaxInstrumentsPerChannel")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-64\": \"X\"}", names),
             SidecarError::SlotOutOfRange);
}

TEST_CASE("parse_sidecar rejects slot 99 (out of range)")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-99\": \"X\"}", names),
             SidecarError::SlotOutOfRange);
}

// ---------------------------------------------------------------------------
// Name-length error
// ---------------------------------------------------------------------------

TEST_CASE("parse_sidecar rejects a value longer than "
          "kMaxInstrumentNameBytes - 1")
{
    RackboyBankNames names{};
    // 16 printable chars + the null terminator = 17 bytes,
    // one past the 16-byte budget.
    CHECK_EQ(parse("{\"PU1-00\": \"0123456789ABCDEF\"}", names),
             SidecarError::NameTooLong);
    // And the reset invariant survives the error return.
    check_all_names_empty(names);
}

TEST_CASE("parse_sidecar accepts a value exactly "
          "kMaxInstrumentNameBytes - 1 bytes long")
{
    // 15 chars fit with room for the null terminator.
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\": \"0123456789ABCDE\"}", names),
             SidecarError::Ok);
    check_name(names.pulse1[0], "0123456789ABCDE");
}

// ---------------------------------------------------------------------------
// Poison-and-check soft-fail invariant
// ---------------------------------------------------------------------------

TEST_CASE("parse_sidecar clears every slot on every error return "
          "(poison-and-check)")
{
    struct Case {
        const char*  label;
        const char*  body;
        SidecarError expected;
    };
    const Case cases[] = {
        {"empty-buffer",      "",                           SidecarError::InvalidJson},
        {"no-brace",          "\"X\"",                      SidecarError::InvalidJson},
        {"trailing-comma",    "{\"PU1-00\": \"X\",}",       SidecarError::InvalidJson},
        {"unknown-chan",      "{\"XX-00\": \"X\"}",         SidecarError::UnknownChannel},
        {"slot-oob",          "{\"PU1-64\": \"X\"}",        SidecarError::SlotOutOfRange},
        {"too-long",          "{\"PU1-00\": \"0123456789ABCDEF\"}", SidecarError::NameTooLong},
    };

    for (const Case& k : cases) {
        CAPTURE(k.label);
        RackboyBankNames names;
        poison_names(names);

        const std::string json = k.body;
        CHECK_EQ(parse(json, names), k.expected);
        check_all_names_empty(names);
    }
}

// ---------------------------------------------------------------------------
// Happy-path parses
// ---------------------------------------------------------------------------

TEST_CASE("parse_sidecar round-trips one PU1 entry")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-03\": \"LEAD\"}", names),
             SidecarError::Ok);
    check_name(names.pulse1[3], "LEAD");
    // Neighbour slots untouched.
    check_name(names.pulse1[2], "");
    check_name(names.pulse1[4], "");
    // Other channels untouched.
    check_name(names.pulse2[3], "");
    check_name(names.wave  [3], "");
    check_name(names.noise [3], "");
}

TEST_CASE("parse_sidecar handles all four channel prefixes")
{
    RackboyBankNames names{};
    const std::string json =
        "{"
        "\"PU1-00\": \"LEAD\","
        "\"PU2-01\": \"CHORD\","
        "\"WV-02\": \"PAD\","
        "\"NO-03\": \"KICK\""
        "}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);

    check_name(names.pulse1[0],  "LEAD");
    check_name(names.pulse2[1],  "CHORD");
    check_name(names.wave  [2],  "PAD");
    check_name(names.noise [3],  "KICK");

    // One adjacent slot per channel stays empty.
    check_name(names.pulse1[1], "");
    check_name(names.pulse2[0], "");
    check_name(names.wave  [3], "");
    check_name(names.noise [2], "");
}

TEST_CASE("parse_sidecar accepts a slot at the upper boundary "
          "(kMaxInstrumentsPerChannel - 1 = 63)")
{
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-63\": \"TOP\"}", names),
             SidecarError::Ok);
    check_name(names.pulse1[63], "TOP");
}

TEST_CASE("parse_sidecar handles an empty name string")
{
    // Empty string values are allowed — they just store an
    // all-zero name, identical to the default.
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-00\": \"\"}", names),
             SidecarError::Ok);
    check_name(names.pulse1[0], "");
}

TEST_CASE("parse_sidecar duplicate keys: last wins")
{
    RackboyBankNames names{};
    const std::string json =
        "{\"PU1-00\": \"FIRST\", \"PU1-00\": \"SECOND\"}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_name(names.pulse1[0], "SECOND");
}

TEST_CASE("parse_sidecar duplicate-key overwrite leaves a "
          "byte-for-byte deterministic POD layout")
{
    // Regression guard for the stale-tail-bytes bug: if
    // store_name only writes through `text[len]` without
    // zeroing the remainder, a shorter second value leaves
    // bytes from the first value behind. C-string readers
    // wouldn't notice, but any POD-wide memcmp / hash /
    // SPSC fingerprint over RackboyBankNames would differ
    // from a fresh parse. check_name_exact inspects every
    // byte up to kMaxInstrumentNameBytes and catches the
    // leak.
    RackboyBankNames names{};
    const std::string json =
        "{\"PU1-00\": \"LONGER_FIRST\", "
        "\"PU1-00\": \"SHORT\"}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_name_exact(names.pulse1[0], "SHORT");
}

TEST_CASE("parse_sidecar routes the same slot number to all four "
          "channel arrays independently")
{
    // Guards store_name's switch against a copy-paste bug
    // like `case kChanWave: target = &out.pulse2[slot];`.
    // The existing "all four channel prefixes" happy-path
    // test uses distinct slot indices per channel, so a
    // cross-wired target pointer would still land in the
    // right per-channel array by accident. This test pins
    // slot 0 on every channel so any misrouting collapses
    // multiple writes into one slot.
    RackboyBankNames names{};
    const std::string json =
        "{"
        "\"PU1-00\": \"A\","
        "\"PU2-00\": \"B\","
        "\"WV-00\":  \"C\","
        "\"NO-00\":  \"D\""
        "}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_name(names.pulse1[0], "A");
    check_name(names.pulse2[0], "B");
    check_name(names.wave  [0], "C");
    check_name(names.noise [0], "D");
}

TEST_CASE("parse_sidecar clears untouched slots on Ok "
          "(poison-and-check on the non-empty happy path)")
{
    // Every other happy-path test starts from
    // RackboyBankNames{}, which makes the "unrelated slots
    // stay empty" assertions tautological — any zero-init
    // slot would pass whether or not the parser ran. Start
    // with a poisoned struct so `names_reset` at the top of
    // parse_sidecar has something to clear, then verify the
    // two populated entries AND that every other slot came
    // out empty.
    RackboyBankNames names;
    poison_names(names);

    const std::string json =
        "{\"PU1-05\": \"LEAD\", \"NO-10\": \"KICK\"}";
    CHECK_EQ(parse(json, names), SidecarError::Ok);

    check_name(names.pulse1[5],  "LEAD");
    check_name(names.noise [10], "KICK");

    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        CAPTURE(i);
        if (i != 5)  check_name(names.pulse1[i], "");
        if (i != 10) check_name(names.noise [i], "");
        check_name(names.pulse2[i], "");
        check_name(names.wave  [i], "");
    }
}

TEST_CASE("parse_sidecar handles inter-token whitespace everywhere")
{
    RackboyBankNames names{};
    const std::string json =
        "{\n"
        "   \"PU1-00\"  :   \"LEAD\"  ,\n"
        "   \"WV-05\"   :   \"PAD\"\n"
        "}\n";
    CHECK_EQ(parse(json, names), SidecarError::Ok);
    check_name(names.pulse1[0], "LEAD");
    check_name(names.wave  [5], "PAD");
}

TEST_CASE("parse_sidecar leaves unrelated slots untouched on Ok")
{
    // Fill only PU1-05 and verify that NO other slot has
    // anything non-zero in its name buffer.
    RackboyBankNames names{};
    CHECK_EQ(parse("{\"PU1-05\": \"HI\"}", names),
             SidecarError::Ok);
    check_name(names.pulse1[5], "HI");

    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        if (i == 5) continue;
        CAPTURE(i);
        check_name(names.pulse1[i], "");
    }
    for (std::size_t i = 0; i < kMaxInstrumentsPerChannel; ++i) {
        CAPTURE(i);
        check_name(names.pulse2[i], "");
        check_name(names.wave  [i], "");
        check_name(names.noise [i], "");
    }
}

// ---------------------------------------------------------------------------
// names_reset + error_string
// ---------------------------------------------------------------------------

TEST_CASE("names_reset clears every byte of every slot")
{
    RackboyBankNames names;
    poison_names(names);
    names_reset(names);
    check_all_names_empty(names);
}

TEST_CASE("sidecar_error_string returns a non-null, distinct, "
          "non-empty string for every error")
{
    const SidecarError all[] = {
        SidecarError::Ok,
        SidecarError::NullJson,
        SidecarError::NullOutNames,
        SidecarError::InvalidJson,
        SidecarError::UnknownChannel,
        SidecarError::SlotOutOfRange,
        SidecarError::NameTooLong,
    };
    constexpr std::size_t kAllCount = sizeof(all) / sizeof(all[0]);

    const char* seen[kAllCount] = {};
    std::size_t seen_count = 0;
    for (SidecarError e : all) {
        const char* s = sidecar_error_string(e);
        REQUIRE(s != nullptr);
        CHECK(s[0] != '\0');
        for (std::size_t i = 0; i < seen_count; ++i) {
            CAPTURE(i);
            CHECK(std::strcmp(seen[i], s) != 0);
        }
        seen[seen_count++] = s;
    }
    CHECK_EQ(seen_count, kAllCount);
}

TEST_CASE("sidecar_error_string returns a non-null string for an "
          "out-of-range enum value")
{
    // Log-replay tooling may deserialize a corrupt int. The
    // fallback in sidecar_error_string must still return a
    // printable string rather than nullptr or crash so the
    // logger can emit "unknown" instead of faulting.
    const auto corrupt = static_cast<SidecarError>(9999);
    const char* s = sidecar_error_string(corrupt);
    REQUIRE(s != nullptr);
    CHECK(s[0] != '\0');
}
