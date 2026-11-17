// rackboy_core/rackboy_clock.cpp
//
// Implementation of Gate 2 tick-all dispatch. See rackboy_clock.h
// for the public API and the AD #10 contract.
//
// Bead rackboy-hvz.

#include "rackboy_clock.h"

#include "rackboy_constants.h"      // kNoInstrument

namespace rackboy {
namespace core {

// Instrument byte offset for the table slot index. Shared by all
// instrument types (Pulse, Wave, Noise) per docs/COMPATIBILITY.md.
static constexpr std::size_t kTableSlotByteOffset = 6;

// Pulse instrument byte 5 bit 4: table-enable flag. When clear, byte 6
// (table_index) is not meaningful and may contain a stale slot value.
// Wave and Noise instruments infer table-enable from non-0xFF table_index.
static constexpr uint8_t kPulseTableEnableBit = 0x10;

// Check whether an instrument's table is enabled, accounting for per-type
// semantics from docs/COMPATIBILITY.md:
//   - Pulse: raw[5] bit 4 must be set AND raw[6] != kNoTable
//   - Wave/Noise: raw[6] != kNoTable (enable inferred from slot value)
static bool instrument_has_table(
    VoiceChannel ch,
    const sav::RackboyInstrument& inst) noexcept
{
    const uint8_t slot = inst.raw[kTableSlotByteOffset];
    if (slot == sav::kNoTable) return false;

    switch (ch) {
    case VoiceChannel::Pulse1:
    case VoiceChannel::Pulse2:
        return (inst.raw[5] & kPulseTableEnableBit) != 0;
    case VoiceChannel::Wave:
    case VoiceChannel::Noise:
        return true;
    }
    return false;
}

// Reuse the same pool lookup helper as trigger_all. Defined as a file-
// local static in both rackboy_trigger.cpp and here to avoid adding a
// shared internal header for a trivial 10-line function. If a third
// caller appears, factor it into a shared helper.
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
    count_out = 0;
    return nullptr;
}

void tick_all(
    VoiceState (&voices)[kNumVoices],
    rackboy::table::TableRegState (&regs)[kNumVoices],
    const sav::RackboyBank& bank,
    rackboy_apu_t* apu) noexcept
{
    // Early out if no bank loaded — same guard as trigger_all.
    if (!bank.loaded) return;

    static constexpr VoiceChannel kChannels[kNumVoices] = {
        VoiceChannel::Pulse1,
        VoiceChannel::Pulse2,
        VoiceChannel::Wave,
        VoiceChannel::Noise,
    };

    for (std::size_t i = 0; i < kNumVoices; ++i) {
        VoiceState& voice = voices[i];
        const VoiceChannel ch = kChannels[i];

        // Skip untriggered voices. A voice that has never received a
        // Gate 1 trigger has no initial NRxx state — advancing its
        // table cursor would produce meaningless register writes.
        if (!voice.triggered) {
            continue;
        }

        // Skip voices with no instrument or out-of-bounds index.
        if (voice.active_instrument_index == kNoInstrument ||
            voice.active_instrument_index < 0) {
            continue;
        }

        uint8_t pool_count = 0;
        const sav::RackboyInstrument* pool = pool_for_channel(ch, bank, pool_count);

        if (static_cast<std::size_t>(voice.active_instrument_index) >= pool_count) {
            continue;
        }

        const sav::RackboyInstrument& inst =
            pool[static_cast<std::size_t>(voice.active_instrument_index)];

        // Check the per-instrument-type table eligibility. Pulse
        // instruments require raw[5] bit 4 (table enable); Wave and
        // Noise infer table enable from a non-0xFF table_index.
        if (!instrument_has_table(ch, inst)) {
            continue;
        }

        const uint8_t table_slot = inst.raw[kTableSlotByteOffset];
        if (table_slot >= sav::kMaxTables) {
            continue;
        }

        const sav::RackboyTable& table = bank.tables[table_slot];

        // Skip if the table slot is not occupied (parser never wrote it).
        if (!table.occupied) {
            continue;
        }

        rackboy::table::table_tick(
            voice,
            regs[i],
            ch,
            table,
            apu,
            &bank);
    }
}

}  // namespace core
}  // namespace rackboy
