// rackboy_sav/rackboy_sav_header.cpp
//
// Implementation of the container-layer validation API declared in
// rackboy_sav_header.h. Bead rackboy-hg5.
//
// This file is deliberately tiny: every check is a single
// byte-compare, in a fixed order that matches the header comment,
// and the first failure short-circuits. No allocation, no STL
// containers, no thrown exceptions.
//
// Order of checks (matters — each assumes the previous passed):
//   1. null pointer
//   2. size
//   3. magic
//   4. active slot in LSDJ's 0..31 range
//   5. active slot == 0 (rackboy v1 only consumes slot 0)
//   6. LSDJ 9.4.0 header-version byte

#include "rackboy_sav_header.h"

#include <cstddef>
#include <cstdint>

namespace rackboy {
namespace sav {

// Compile-time bounds proof: every offset this file reads is
// strictly below `kSavSizeBytes`, so the `len != kSavSizeBytes`
// equality guard inside validate_header_impl is a complete proof
// that every `buf[offset]` read is in range. If a future bead bumps
// any of these offsets, the assertion fires at compile time instead
// of letting a runtime read slip past the size check.
static_assert(kMagicOffset + 1      < kSavSizeBytes,
              "kMagicOffset + 1 must fit inside an LSDJ .sav.");
static_assert(kActiveSlotOffset     < kSavSizeBytes,
              "kActiveSlotOffset must fit inside an LSDJ .sav.");
static_assert(kHeaderVersionOffset  < kSavSizeBytes,
              "kHeaderVersionOffset must fit inside an LSDJ .sav.");

namespace {

// Core validation routine. `out` may be null — the info fields are
// only populated on a successful (Ok) return, and only when the
// caller actually provided a destination.
HeaderError validate_header_impl(const std::uint8_t* buf,
                                 std::size_t         len,
                                 HeaderInfo*         out) noexcept
{
    // (1) Null pointer. Reject early so every later read is safe.
    if (buf == nullptr) {
        return HeaderError::NullBuffer;
    }

    // (2) Size. LSDJ .savs are always exactly 128 KiB. We do not
    //     try to salvage a partial buffer because every downstream
    //     offset we care about lives inside the 128 KiB range, and
    //     a shorter buffer would force conditional offset math in
    //     every subsequent read.
    if (len != kSavSizeBytes) {
        return HeaderError::SizeMismatch;
    }

    // Every `buf[offset]` read below is in-range: the file-scope
    // static_asserts above prove that every offset constant is
    // strictly less than kSavSizeBytes, and the `len !=
    // kSavSizeBytes` check above rejected any buffer that is not
    // exactly kSavSizeBytes long. No runtime bounds check needed.

    // (3) Magic. Two bytes at kMagicOffset must be 'j' 'k'.
    const std::uint8_t magic0 = buf[kMagicOffset + 0];
    const std::uint8_t magic1 = buf[kMagicOffset + 1];
    const bool magic_ok =
        (magic0 == kMagicByte0) && (magic1 == kMagicByte1);
    if (!magic_ok) {
        return HeaderError::MagicMismatch;
    }

    // (4) Active slot in LSDJ's 0..31 range.
    const std::uint8_t active_slot = buf[kActiveSlotOffset];
    if (active_slot >= kLsdjMaxSlotExclusive) {
        return HeaderError::ActiveSlotOutOfRange;
    }

    // (5) Active slot == 0 (rackboy v1 only consumes slot 0).
    if (active_slot != kRackboyV1ActiveSlot) {
        return HeaderError::ActiveSlotNotZero;
    }

    // (6) LSDJ 9.4.0 header version byte at 0x8100.
    const std::uint8_t header_version = buf[kHeaderVersionOffset];
    if (header_version != kHeaderVersionLsdj940) {
        return HeaderError::VersionMismatch;
    }

    // Success. Populate `out` only now, so a caller that uses
    // `read_header(buf, len, &info)` can trust that `info` is
    // untouched on any non-Ok return.
    if (out != nullptr) {
        out->active_slot    = active_slot;
        out->header_version = header_version;
        out->magic_ok       = magic_ok;
    }
    return HeaderError::Ok;
}

}  // namespace

HeaderError validate_header(const std::uint8_t* buf,
                            std::size_t         len) noexcept
{
    return validate_header_impl(buf, len, nullptr);
}

HeaderError read_header(const std::uint8_t* buf,
                        std::size_t         len,
                        HeaderInfo*         out) noexcept
{
    return validate_header_impl(buf, len, out);
}

const char* header_error_string(HeaderError e) noexcept
{
    // Intentionally a switch rather than a table so a future
    // enum-value addition fails `-Wswitch` / `-Wswitch-enum` loudly
    // instead of silently returning a default string.
    switch (e) {
        case HeaderError::Ok:
            return "ok";
        case HeaderError::NullBuffer:
            return "null buffer";
        case HeaderError::SizeMismatch:
            return "size mismatch (expected 131072 bytes)";
        case HeaderError::MagicMismatch:
            return "LSDJ magic 'jk' missing at 0x813E";
        case HeaderError::ActiveSlotOutOfRange:
            return "active slot byte at 0x8140 out of LSDJ 0..31 range";
        case HeaderError::ActiveSlotNotZero:
            return "active slot is not 0 (rackboy v1 only reads slot 0)";
        case HeaderError::VersionMismatch:
            return "header version byte at 0x8100 != 0x1B (LSDJ 9.4.0)";
    }
    // No default: every enumerator is covered above. This line is
    // only reached if a caller casts a bogus int into HeaderError,
    // which is already undefined per the standard.
    return "unknown HeaderError value";
}

// Compile-time bounds proof for first_block_for_slot: the function
// reads [kBlockAllocTableOffset, kBlockAllocTableOffset +
// kBlockAllocTableLen). Both ends must fit inside the documented
// 131072-byte LSDJ sav so a caller who passed a full-size buffer
// (the expected case) never reads past the end.
static_assert(kBlockAllocTableOffset + kBlockAllocTableLen
                  <= kSavSizeBytes,
              "block alloc table must fit inside an LSDJ .sav.");

std::size_t first_block_for_slot(const std::uint8_t* buf,
                                 std::uint8_t        slot) noexcept
{
    if (buf == nullptr) {
        return kBlockAllocTableLen;
    }
    for (std::size_t i = 0; i < kBlockAllocTableLen; ++i) {
        if (buf[kBlockAllocTableOffset + i] == slot) {
            return i;
        }
    }
    return kBlockAllocTableLen;
}

}  // namespace sav
}  // namespace rackboy
