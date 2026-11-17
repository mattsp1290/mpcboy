// channel_io.cpp — see channel_io.h. mpcboy-owned; extends the vendored table
// interpreter's types.

#include "channel_io.h"

#include "rackboy_apu.h"
#include "rackboy_pitch.h"

namespace rackboy {
namespace table {

namespace {

inline int16_t cents_to_nearest_semitone(int32_t cents) noexcept
{
    const int32_t c = cents + 50;
    int32_t semis = (c >= 0) ? (c / 100) : -((-c + 99) / 100);
    if (semis > 32767) semis = 32767;
    if (semis < -32768) semis = -32768;
    return static_cast<int16_t>(semis);
}

// Width bit for derived NR43 values: the flag wins, otherwise the instrument's
// own shape bit is kept.
inline uint8_t noise_derived_width_bit(const TableRegState& regs) noexcept
{
    return (regs.noise_flags & kNoiseWidth7) ? 0x08u
                                              : static_cast<uint8_t>(regs.nr43_shape & 0x08u);
}

// Reload the configured length before a retrigger. On hardware a trigger reloads
// the length counter to its maximum only when it has run out, so without this a
// retrigger after the note's Length expired would sound for 64/256 s (pulse,
// noise) or 1 s (wave) instead of Length. Rewriting NRx1 / NR31 restarts the
// counter at the configured value; the pulse duty bits are kept.
void reload_length(const TableRegState& regs, core::VoiceChannel ch, rackboy_apu_t* apu) noexcept
{
    if (!regs.length_enable) return;
    const ChannelRegs& r = channel_regs(ch);
    switch (ch) {
    case core::VoiceChannel::Pulse1:
    case core::VoiceChannel::Pulse2:
        rackboy_apu_write_reg(apu, r.nrx1, static_cast<uint8_t>(regs.duty | regs.len_lo6));
        break;
    case core::VoiceChannel::Wave:
        rackboy_apu_write_reg(apu, r.nrx1, regs.len_nr31);
        break;
    case core::VoiceChannel::Noise:
        rackboy_apu_write_reg(apu, r.nrx1, regs.len_lo6);
        break;
    }
}

// Frequency write without the trigger bit: NRx3 low byte, NRx4 high bits (plus
// length enable, so a running length counter keeps running across ticks).
void write_freq_no_trigger(rackboy_apu_t* apu, const ChannelRegs& r, uint16_t freq,
                           uint8_t length_enable) noexcept
{
    rackboy_apu_write_reg(apu, r.nrx3, static_cast<uint8_t>(freq & 0xFF));
    rackboy_apu_write_reg(
        apu, r.nrx4,
        static_cast<uint8_t>(((freq >> 8) & kFreqHiMask) |
                             (length_enable ? kLengthEnableBit : 0)));
}

}  // namespace

uint8_t noise_width_bits(const TableRegState& regs) noexcept
{
    return (regs.noise_flags & kNoiseWidth7) ? 0x08u : 0u;
}

uint8_t trigger_byte(const TableRegState& regs, core::VoiceChannel ch) noexcept
{
    const uint8_t le = regs.length_enable ? kLengthEnableBit : 0;
    if (ch == core::VoiceChannel::Noise)
        return static_cast<uint8_t>(kTriggerBit | le);
    return static_cast<uint8_t>(kTriggerBit | ((regs.freq_11bit >> 8) & kFreqHiMask) | le);
}

void retrigger(const TableRegState& regs, core::VoiceChannel ch, rackboy_apu_t* apu) noexcept
{
    reload_length(regs, ch, apu);
    rackboy_apu_write_reg(apu, channel_regs(ch).nrx4, trigger_byte(regs, ch));
}

void write_envelope_retrigger(uint8_t value, TableRegState& regs, core::VoiceChannel ch,
                              rackboy_apu_t* apu) noexcept
{
    reload_length(regs, ch, apu);
    const ChannelRegs& r = channel_regs(ch);
    if (ch == core::VoiceChannel::Wave) {
        regs.wave_volume = value;
        rackboy_apu_write_reg(apu, r.nrx2, static_cast<uint8_t>(value & kNR32OutputLevelMask));
    } else {
        regs.envelope = value;
        rackboy_apu_write_reg(apu, r.nrx2, value);
    }
    rackboy_apu_write_reg(apu, r.nrx4, trigger_byte(regs, ch));
}

void kill_channel(core::VoiceState& voice, TableRegState& regs, core::VoiceChannel ch,
                  rackboy_apu_t* apu) noexcept
{
    if (ch == core::VoiceChannel::Wave) {
        regs.wave_volume = 0;
        // CH3: turn off the DAC by clearing NR30 bit 7.
        rackboy_apu_write_reg(apu, kNR30, 0x00);
    } else {
        regs.envelope = 0;
        rackboy_apu_write_reg(apu, channel_regs(ch).nrx2, 0x00);
    }
    voice.triggered = false;
    voice.kill_ticks_remaining = 0;
}

// Pitch composition, in cents:
//   total = (pitch_offset + vibrato) * 100
//         + pbend_accum * 100 / 16      (`P` command, 1/16 semitone units)
//         + fine_cents + bend_cents
// pitch_offset already holds the accumulated transposes and the `L` slide. With
// fine = bend = 0 the cents are a multiple of 100 and the register values are
// bit-identical to the old semitone functions.
//
// `trigger` writes the trigger bit with the frequency (table_trigger); without
// it only pitch bits are written. regs.engine_mode (table_trigger called with
// TriggerExtras) selects the new noise rule; nullptr extras keep the legacy
// "semitones == 0 uses raw[4]" rule so the vendored tests are unaffected.
void write_channel_pitch(rackboy_apu_t* apu, core::VoiceChannel ch, TableRegState& regs,
                         int16_t vibrato_semis, int16_t pbend_accum, bool trigger) noexcept
{
    const int32_t cents =
        (static_cast<int32_t>(regs.pitch_offset) + vibrato_semis) * 100 +
        static_cast<int32_t>(pbend_accum) * 100 / 16 +
        static_cast<int32_t>(regs.fine_cents) +
        static_cast<int32_t>(regs.bend_cents);
    const ChannelRegs& r = channel_regs(ch);

    if (ch != core::VoiceChannel::Noise) {
        regs.freq_11bit = (ch == core::VoiceChannel::Wave)
                              ? core::pitch_wave_11bit_cents(cents)
                              : core::pitch_square_11bit_cents(cents);
        if (trigger) {
            rackboy_apu_write_reg(apu, r.nrx3, static_cast<uint8_t>(regs.freq_11bit & 0xFF));
            rackboy_apu_write_reg(apu, r.nrx4, trigger_byte(regs, ch));
        } else {
            write_freq_no_trigger(apu, r, regs.freq_11bit, regs.length_enable);
        }
        return;
    }

    const bool fixed = (regs.noise_flags & kNoiseFixed) != 0;
    uint8_t nr43;
    if (fixed) {
        // Fixed noise: only table_trigger and `N` write NR43.
        if (!trigger) return;
        nr43 = static_cast<uint8_t>(regs.nr43_shape | noise_width_bits(regs));
    } else {
        const int16_t semis = cents_to_nearest_semitone(cents);
        if (trigger && !regs.engine_mode) {
            // Legacy trigger rule (vendored behaviour): the instrument's raw[4]
            // is authoritative at zero offset.
            nr43 = (semis == 0) ? regs.nr43_shape : core::pitch_noise_nr43(semis);
        } else {
            // Track mode: always derived from the pitch; width merged in.
            nr43 = static_cast<uint8_t>((core::pitch_noise_nr43(semis) & ~0x08u) |
                                        noise_derived_width_bit(regs));
        }
    }
    rackboy_apu_write_reg(apu, kNR43, nr43);
    if (trigger) rackboy_apu_write_reg(apu, r.nrx4, trigger_byte(regs, ch));
}

}  // namespace table
}  // namespace rackboy
