// src/engine/instrument_encoder.h - ChannelSettings -> LSDJ-layout RackboyInstrument.
#pragma once

#include "channel_settings.h"
#include "rackboy_sav_types.h"
#include "rackboy_table.h"
#include "rackboy_voice.h"

namespace mpcboy {

// Encodes the 16-byte instrument the vendored table_trigger reads:
//   Pulse: [1] envelope (NRx2), [4] sweep (NR10, Pulse 1 only), [5] bit 4 table
//          enable, [6] table slot (kNoTable if none), [7] duty bits 7-6 | pan bits 1-0
//          (finetune bits 5-2 stay 0: fine tune travels in TriggerExtras as cents).
//   Wave:  [1] NR32 level in bits 6-5, [6] table slot, [8] wave slot.
//   Noise: [1] envelope, [4] NR43 (Track: pitch_noise_nr43(0); Fixed: the fixed shape;
//          bit 3 = width), [6] table slot.
// raw[3] (LSDJ length) is never used: length comes from TriggerExtras.
rackboy::sav::RackboyInstrument encode_instrument(rackboy::core::VoiceChannel ch,
                                                  const ChannelSettings& s);

// NR32 level code for a wave level (0 mute, 1 = 100 %, 2 = 50 %, 3 = 25 %).
uint8_t wave_level_code(WaveLevel l);

// The companion data for table_trigger.
rackboy::table::TriggerExtras encode_trigger_extras(rackboy::core::VoiceChannel ch,
                                                    const ChannelSettings& s,
                                                    int bendCents);

// NR51 bits for one channel (0..3) and pan.
uint8_t pan_nr51_bits(int channel, Pan p);

}  // namespace mpcboy
