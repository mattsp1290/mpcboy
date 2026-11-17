// rackboy_sav/rackboy_sav_instruments.cpp
//
// Implementation of the instrument block parser declared in
// rackboy_sav_instruments.h. Bead rackboy-1ub.
//
// The whole function is intentionally linear: reset the bank,
// walk the 64-byte allocation map once validating each byte,
// walk it again classifying allocated slots into per-channel
// pools, then stamp `loaded = true`. No allocation, no
// exceptions, no recursion, no backwards jumps.

#include "rackboy_sav_instruments.h"

#include <cstddef>
#include <cstdint>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

namespace {

// Copy 16 bytes from `src` into `dst.raw`. Split into a helper
// so the per-channel pool-append sites stay readable. The body
// is a plain loop rather than `std::memcpy` because
// RackboyInstrument is trivially copyable and the compiler
// lowers the loop to a memcpy at -O1 and above; keeping the loop
// avoids the <cstring> include and the debug-build call.
inline void copy_record(RackboyInstrument& dst,
                        const std::uint8_t* src) noexcept
{
    for (std::size_t k = 0; k < kInstrumentRecordBytes; ++k) {
        dst.raw[k] = src[k];
    }
}

}  // namespace

InstrumentsError parse_instruments(const std::uint8_t* song,
                                   std::size_t         song_len,
                                   RackboyBank*        out_bank) noexcept
{
    // (1) Parameter-level guards. Do these before bank_reset so a
    //     caller that passes a null bank cannot have its memory
    //     touched.
    if (out_bank == nullptr) {
        return InstrumentsError::NullOutBank;
    }
    if (song == nullptr) {
        bank_reset(*out_bank);
        return InstrumentsError::NullSong;
    }
    if (song_len < kDecompressedSongSizeBytes) {
        bank_reset(*out_bank);
        return InstrumentsError::SongTooShort;
    }

    // (2) Reset the destination bank. Every subsequent pool-
    //     append operates on a known-clean dense prefix, and
    //     every failure path below leaves the bank in the reset
    //     state because the pool_* counts are still zero.
    bank_reset(*out_bank);

    // (3) First pass: validate the allocation map. LSDJ only
    //     writes 0x00 or 0x01 into these bytes, so anything else
    //     is a corrupt song. Catching this in its own pass keeps
    //     the classification pass below free of alloc-map
    //     branching.
    for (std::size_t slot = 0; slot < kInstrumentAllocMapLen; ++slot) {
        const std::uint8_t byte =
            song[kInstrumentAllocMapOffset + slot];
        if (byte != kInstrumentAllocated
         && byte != kInstrumentUnallocated) {
            // Bank already reset; nothing to clean up.
            return InstrumentsError::AllocMapInvalid;
        }
    }

    // (4) Second pass: classify + copy allocated slots. Walk the
    //     slots in allocation-map order so the dense-prefix
    //     invariant on each per-channel pool is respected (slot
    //     index `i < *_count` for channel C is valid).
    for (std::size_t slot = 0; slot < kInstrumentAllocMapLen; ++slot) {
        const std::uint8_t alloc_byte =
            song[kInstrumentAllocMapOffset + slot];
        if (alloc_byte != kInstrumentAllocated) {
            continue;
        }

        // Pointer to the 16-byte record for this slot. The
        // `kInstrumentParamsEnd` static_assert in the header
        // proves every record lives inside the decompressed
        // song blob, so the size check at step (1) is a
        // complete bounds proof for the read below.
        const std::uint8_t* const record =
            song + kInstrumentParamsOffset
                 + slot * kInstrumentSlotStride;
        const std::uint8_t type_byte = record[0];

        switch (type_byte) {
            case kInstrumentTypePulse: {
                // Every Pulse instrument lands in pulse1_pool for
                // v1. A future bead will split between
                // pulse1_pool and pulse2_pool based on which
                // channel the song's chains / phrases actually
                // play the instrument on. See the header
                // channel-classification note.
                //
                // Defensive: the
                // `kInstrumentAllocMapLen <= kMaxInstrumentsPerChannel`
                // static_assert in the header makes this branch
                // unreachable for any v1 layout (the pool is
                // sized to fit every slot in any one channel).
                // Kept so a future bump of either constant that
                // breaks the invariant fires the soft-fail path
                // instead of writing past the pool.
                if (out_bank->pulse1_count
                    >= kMaxInstrumentsPerChannel) {
                    bank_reset(*out_bank);
                    return InstrumentsError::ChannelPoolFull;
                }
                copy_record(
                    out_bank->pulse1_pool[out_bank->pulse1_count],
                    record);
                ++out_bank->pulse1_count;
                break;
            }

            case kInstrumentTypeWave: {
                if (out_bank->wave_count
                    >= kMaxInstrumentsPerChannel) {
                    bank_reset(*out_bank);
                    return InstrumentsError::ChannelPoolFull;
                }
                copy_record(
                    out_bank->wave_pool[out_bank->wave_count],
                    record);
                ++out_bank->wave_count;
                break;
            }

            case kInstrumentTypeKit: {
                // Deliberate v1 skip. LSDJ kits reference PCM
                // sample data in ROM, which rackboy v1 does not
                // model. The slot is simply not added to any
                // pool; the alloc-map walk continues.
                break;
            }

            case kInstrumentTypeNoise: {
                if (out_bank->noise_count
                    >= kMaxInstrumentsPerChannel) {
                    bank_reset(*out_bank);
                    return InstrumentsError::ChannelPoolFull;
                }
                copy_record(
                    out_bank->noise_pool[out_bank->noise_count],
                    record);
                ++out_bank->noise_count;
                break;
            }

            default: {
                // Unknown type byte. Reject rather than silently
                // drop — the caller's UI should surface the
                // malformed-song message.
                bank_reset(*out_bank);
                return InstrumentsError::UnknownType;
            }
        }
    }

    // (5) Publish. `loaded = true` is the last field we touch so
    //     an audio thread that somehow races the swap observes a
    //     fully-populated bank or a freshly-reset one, never a
    //     half-populated state.
    out_bank->loaded = true;
    return InstrumentsError::Ok;
}

const char* instruments_error_string(InstrumentsError e) noexcept
{
    // No default case on purpose — `-Wswitch-enum` catches a
    // future enumerator addition at compile time.
    switch (e) {
        case InstrumentsError::Ok:
            return "ok";
        case InstrumentsError::NullSong:
            return "null decompressed-song buffer";
        case InstrumentsError::NullOutBank:
            return "null output bank";
        case InstrumentsError::SongTooShort:
            return "song buffer shorter than 32 KiB";
        case InstrumentsError::AllocMapInvalid:
            return "instrument alloc map byte != 0x00 and != 0x01";
        case InstrumentsError::UnknownType:
            return "instrument type byte not in {Pulse, Wave, Kit, Noise}";
        case InstrumentsError::ChannelPoolFull:
            return "per-channel instrument pool exceeded 64 slots";
    }
    return "unknown InstrumentsError value";
}

}  // namespace sav
}  // namespace rackboy
