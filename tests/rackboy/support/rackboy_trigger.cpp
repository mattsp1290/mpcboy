// rackboy_core/rackboy_trigger.cpp
//
// Implementation of Gate 1 trigger-all dispatch. See rackboy_trigger.h
// for the public API and the AD #9 contract.
//
// Bead rackboy-bfl.

#include "rackboy_trigger.h"

#include "rackboy_constants.h"      // kNoInstrument

namespace rackboy {
namespace core {

// Channel-to-pool helper: given a channel enum and a bank, return a
// pointer to the first instrument in the matching pool and the pool's
// count. Keeps the per-channel dispatch in trigger_all() free of
// repetitive switch arms for pool selection.
static const sav::RackboyInstrument* pool_for_channel(
    VoiceChannel ch,
    const sav::RackboyBank& bank,
    uint8_t& count_out) noexcept
{
    switch (ch) {
    case VoiceChannel::Pulse1:
        count_out = bank.pulse1_count;
        return bank.pulse1_pool;
    case VoiceChannel::Pulse2:
        count_out = bank.pulse2_count;
        return bank.pulse2_pool;
    case VoiceChannel::Wave:
        count_out = bank.wave_count;
        return bank.wave_pool;
    case VoiceChannel::Noise:
        count_out = bank.noise_count;
        return bank.noise_pool;
    }
    // Unreachable — the enum is exhaustive. Silence compiler warnings
    // on toolchains that do not treat covered-switch as exhaustive.
    count_out = 0;
    return nullptr;
}

void trigger_all(
    VoiceState (&voices)[kNumVoices],
    rackboy::table::TableRegState (&regs)[kNumVoices],
    const sav::RackboyBank& bank,
    rackboy_apu_t* apu) noexcept
{
    // Early out if the bank was never successfully parsed. bank_reset()
    // zeroes all pool counts so the per-voice bounds check would skip
    // every voice anyway, but this guard decouples trigger_all's safety
    // from the parser's bank-reset-on-failure invariant chain.
    if (!bank.loaded) return;

    // Channel ordering matches the VoiceChannel enum's underlying values
    // (0..3) so the loop index doubles as both an array index and the
    // enum cast target.
    static constexpr VoiceChannel kChannels[kNumVoices] = {
        VoiceChannel::Pulse1,
        VoiceChannel::Pulse2,
        VoiceChannel::Wave,
        VoiceChannel::Noise,
    };

    for (std::size_t i = 0; i < kNumVoices; ++i) {
        VoiceState& voice = voices[i];
        const VoiceChannel ch = kChannels[i];

        // Skip voices with no instrument selected — they produce silence
        // and should not touch APU registers.
        if (voice.active_instrument_index == kNoInstrument) {
            continue;
        }

        // Look up the instrument from the bank's per-channel pool.
        uint8_t pool_count = 0;
        const sav::RackboyInstrument* pool = pool_for_channel(ch, bank, pool_count);

        // Bounds check: if the active index somehow exceeds the pool
        // count (e.g. the pool shrank after a bank swap but the voice
        // has not been re-mapped yet), skip this voice rather than
        // reading out-of-bounds memory. The control-to-index hysteresis
        // mapping would normally prevent this, but a defensive check
        // here costs almost nothing on the audio thread.
        if (voice.active_instrument_index < 0 ||
            static_cast<std::size_t>(voice.active_instrument_index) >= pool_count) {
            continue;
        }

        const sav::RackboyInstrument& inst =
            pool[static_cast<std::size_t>(voice.active_instrument_index)];

        rackboy::table::table_trigger(
            voice,
            regs[i],
            ch,
            inst,
            voice.target_pitch,
            apu);
    }
}

}  // namespace core
}  // namespace rackboy
