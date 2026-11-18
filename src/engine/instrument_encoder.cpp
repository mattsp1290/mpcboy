#include "instrument_encoder.h"

#include <algorithm>
#include <cstring>

#include "rackboy_pitch.h"

namespace mpcboy {

using rackboy::core::VoiceChannel;

uint8_t wave_level_code(WaveLevel l)
{
    switch (l)
    {
        case WaveLevel::P100: return 1;
        case WaveLevel::P50: return 2;
        case WaveLevel::P25: return 3;
        case WaveLevel::Mute: break;
    }
    return 0;
}

int waveLevelPercent(WaveLevel l)
{
    switch (l)
    {
        case WaveLevel::P100: return 100;
        case WaveLevel::P50: return 50;
        case WaveLevel::P25: return 25;
        case WaveLevel::Mute: break;
    }
    return 0;
}

WaveLevel waveLevelFromPercent(int pct)
{
    return pct >= 75 ? WaveLevel::P100 : (pct >= 38 ? WaveLevel::P50 : WaveLevel::P25);
}

uint8_t pan_nr51_bits(int channel, Pan p)
{
    const uint8_t right = static_cast<uint8_t>(1u << channel);
    const uint8_t left = static_cast<uint8_t>(0x10u << channel);
    switch (p)
    {
        case Pan::L: return left;
        case Pan::R: return right;
        case Pan::C: return static_cast<uint8_t>(left | right);
        case Pan::Off: break;
    }
    return 0;
}

static uint8_t envelope_byte(const ChannelSettings& s)
{
    return static_cast<uint8_t>((std::clamp<int>(s.volume, 0, 15) << 4) | (s.envUp ? 0x08 : 0x00) |
                                std::clamp<int>(s.envPeriod, 0, 7));
}

rackboy::sav::RackboyInstrument encode_instrument(VoiceChannel ch, const ChannelSettings& s)
{
    rackboy::sav::RackboyInstrument inst;
    std::memset(inst.raw, 0, sizeof(inst.raw));
    const uint8_t table = s.tableSlot >= 0 && s.tableSlot < 16
                              ? static_cast<uint8_t>(s.tableSlot)
                              : rackboy::sav::kNoTable;
    inst.raw[6] = table;

    switch (ch)
    {
        case VoiceChannel::Pulse1:
        case VoiceChannel::Pulse2:
            inst.raw[1] = envelope_byte(s);
            if (ch == VoiceChannel::Pulse1)
            {
                // The sweep is "off" when either its time or its amount (shift) is 0, and must then be
                // written fully disabled (NR10 = 0). A sweep unit with time > 0 and shift 0 would still
                // run the frequency-overflow check (freq + freq >> 0 = 2 x freq, SameBoy apu.c
                // sweep_calculation_done) and silence high notes; shift > 0 with time 0 runs it at trigger.
                const int time = std::clamp<int>(s.sweepTime, 0, 7);
                const int shift = std::clamp<int>(s.sweepShift, 0, 7);
                inst.raw[4] = (time == 0 || shift == 0)
                                  ? uint8_t{0}
                                  : static_cast<uint8_t>((time << 4) | (s.sweepDown ? 0x08 : 0x00) | shift);
            }
            inst.raw[5] = table != rackboy::sav::kNoTable ? 0x10 : 0x00;
            inst.raw[7] = static_cast<uint8_t>((std::clamp<int>(s.duty, 0, 3) << 6) |
                                               (static_cast<uint8_t>(s.pan) & 0x03));
            break;
        case VoiceChannel::Wave:
            inst.raw[1] = static_cast<uint8_t>(wave_level_code(s.waveLevel) << 5);
            inst.raw[8] = static_cast<uint8_t>(std::clamp<int>(s.waveSlot, 0, 15));
            break;
        case VoiceChannel::Noise:
        {
            inst.raw[1] = envelope_byte(s);
            uint8_t nr43;
            if (s.noisePitchMode == NoisePitchMode::Fixed)
            {
                const int idx = std::clamp<int>(s.noiseFixedShape, 0, 111);
                nr43 = static_cast<uint8_t>(((idx / 8) << 4) | (idx % 8));
            }
            else
            {
                nr43 = rackboy::core::pitch_noise_nr43(0);
            }
            if (s.noiseWidth7) nr43 |= 0x08;
            inst.raw[4] = nr43;
            break;
        }
    }
    return inst;
}

rackboy::table::TriggerExtras encode_trigger_extras(VoiceChannel ch, const ChannelSettings& s, int bendCents)
{
    rackboy::table::TriggerExtras e;
    e.fine_cents = static_cast<int8_t>(std::clamp<int>(s.fineCents, -50, 50));
    e.bend_cents = static_cast<int16_t>(bendCents);
    e.noise_flags = 0;
    if (ch == VoiceChannel::Noise)
    {
        if (s.noisePitchMode == NoisePitchMode::Fixed) e.noise_flags |= rackboy::table::kNoiseFixed;
        if (s.noiseWidth7) e.noise_flags |= rackboy::table::kNoiseWidth7;
    }
    e.length = static_cast<uint8_t>(std::clamp<int>(s.length, 0, 64));
    return e;
}

}  // namespace mpcboy
