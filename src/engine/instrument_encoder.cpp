#include "instrument_encoder.h"

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

static uint8_t clampu(int v, int lo, int hi) { return static_cast<uint8_t>(v < lo ? lo : (v > hi ? hi : v)); }

static uint8_t envelope_byte(const ChannelSettings& s)
{
    return static_cast<uint8_t>((clampu(s.volume, 0, 15) << 4) | (s.envUp ? 0x08 : 0x00) |
                                clampu(s.envPeriod, 0, 7));
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
                inst.raw[4] = static_cast<uint8_t>((clampu(s.sweepTime, 0, 7) << 4) |
                                                   (s.sweepDown ? 0x08 : 0x00) |
                                                   clampu(s.sweepShift, 0, 7));
            inst.raw[5] = table != rackboy::sav::kNoTable ? 0x10 : 0x00;
            inst.raw[7] = static_cast<uint8_t>((clampu(s.duty, 0, 3) << 6) |
                                               (static_cast<uint8_t>(s.pan) & 0x03));
            break;
        case VoiceChannel::Wave:
            inst.raw[1] = static_cast<uint8_t>(wave_level_code(s.waveLevel) << 5);
            inst.raw[8] = clampu(s.waveSlot, 0, 15);
            break;
        case VoiceChannel::Noise:
        {
            inst.raw[1] = envelope_byte(s);
            uint8_t nr43;
            if (s.noisePitchMode == NoisePitchMode::Fixed)
            {
                const int idx = clampu(s.noiseFixedShape, 0, 111);
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
    e.fine_cents = static_cast<int8_t>(s.fineCents < -50 ? -50 : (s.fineCents > 50 ? 50 : s.fineCents));
    e.bend_cents = static_cast<int16_t>(bendCents);
    e.noise_flags = 0;
    if (ch == VoiceChannel::Noise)
    {
        if (s.noisePitchMode == NoisePitchMode::Fixed) e.noise_flags |= rackboy::table::kNoiseFixed;
        if (s.noiseWidth7) e.noise_flags |= rackboy::table::kNoiseWidth7;
    }
    e.length = clampu(s.length, 0, 64);
    return e;
}

}  // namespace mpcboy
