// rackboy_sav/rackboy_sav_waves.cpp
//
// Implementation of the CH3 wave frame pool parser declared in
// rackboy_sav_waves.h. Bead rackboy-ntq.
//
// The function shape is: null guards -> clear the bank's
// `waves[]` array -> walk 255 consecutive 16-byte slots,
// copying each and inferring occupancy from whether any byte
// is non-zero. No allocation, no exceptions, no STL
// containers, no recursion.

#include "rackboy_sav_waves.h"

#include <cstddef>
#include <cstdint>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

WavesError parse_waves(const std::uint8_t* song,
                       std::size_t         song_len,
                       RackboyBank*        out_bank) noexcept
{
    // (1) Parameter-level guard: out_bank first so a null bank
    //     pointer cannot have its memory touched.
    if (out_bank == nullptr) {
        return WavesError::NullOutBank;
    }

    // (2) Reset the waves[] sub-array unconditionally (only
    //     after the out_bank null check). Every code path
    //     below this line leaves waves[] in a known state —
    //     either freshly cleared (on any error return) or
    //     fully populated (on Ok). Instrument pools, tables[],
    //     and the `loaded` flag are NEVER touched: that is the
    //     responsibility of parse_instruments / parse_tables.
    for (std::size_t k = 0; k < kTotalWaveFrameSlots; ++k) {
        wave_frame_reset(out_bank->waves[k]);
    }

    // (3) Song-side guards. Placed AFTER the wave_frame_reset
    //     loop so a caller who poisoned waves[] still gets a
    //     cleared sub-array on every error path.
    if (song == nullptr) {
        return WavesError::NullSong;
    }
    if (song_len < kDecompressedSongSizeBytes) {
        return WavesError::SongTooShort;
    }

    // (4) Walk the 255 slots. For each slot, copy the 16
    //     on-disk bytes and track whether any byte was
    //     non-zero. An all-zero slot stays in its just-reset
    //     state (samples zeroed, occupied = false). The
    //     static_asserts in the header prove every song[offset]
    //     read is in-bounds after the size check above.
    for (std::size_t k = 0; k < kTotalWaveFrameSlots; ++k) {
        const std::size_t base =
            kWaveFramePoolOffset + k * kWaveFrameSlotStride;

        RackboyWaveFrame& frame_out = out_bank->waves[k];

        bool any_nonzero = false;
        for (std::size_t b = 0; b < kWaveFrameSlotStride; ++b) {
            const std::uint8_t byte = song[base + b];
            frame_out.samples[b] = byte;
            if (byte != 0) {
                any_nonzero = true;
            }
        }

        // Occupancy is inferred by content: an all-zero slot
        // stays in its just-reset (unoccupied, samples zeroed)
        // state, matching the "holes in the wave pool"
        // semantics documented in rackboy_sav_waves.h.
        // Populated slots close with `occupied = true` after
        // the sample writes so a single-threaded invariant
        // holds: any slot with `occupied == true` has its 16
        // sample bytes fully written. The audio-thread
        // visibility guarantee comes from the SPSC pointer
        // swap in rackboy-2i8, not from in-parse ordering.
        if (any_nonzero) {
            frame_out.occupied = true;
        }
    }

    return WavesError::Ok;
}

const char* waves_error_string(WavesError e) noexcept
{
    // No default case on purpose — -Wswitch-enum catches a
    // future enumerator addition at compile time.
    switch (e) {
        case WavesError::Ok:
            return "ok";
        case WavesError::NullSong:
            return "null decompressed-song buffer";
        case WavesError::NullOutBank:
            return "null output bank";
        case WavesError::SongTooShort:
            return "song buffer shorter than 32 KiB";
    }
    // Fallback is only reachable if a caller cast an arbitrary
    // int to WavesError — e.g. log-replay tooling
    // deserializing a corrupt log line. The WavesError enum
    // value contract in the header pins enumerators 0..3, so
    // any other value is a deserialization bug. We still
    // return a string instead of asserting because logging
    // tools would rather print "unknown" than crash.
    return "unknown WavesError value";
}

}  // namespace sav
}  // namespace rackboy
