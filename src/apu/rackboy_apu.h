/*
 * rackboy_apu/rackboy_apu.h
 *
 * Public driver API for rackboy's Game Boy APU. Wraps the vendored
 * SameBoy APU (`vendor/sameboy/apu.c`) behind the rackboy_apu shim
 * and exposes three operations:
 *
 *   rackboy_apu_init         — reset APU state, set sample rate
 *   rackboy_apu_write_reg    — write a CPU-addressed APU register
 *   rackboy_apu_run_samples  — render N output samples, per-channel
 *
 * Per Architecture Decision D-009, v1 uses 4-pass mute rendering:
 * run the APU four times per buffer, each time muting three channels
 * via upstream `GB_set_channel_muted` and capturing the fourth. This
 * means 4x APU CPU cost per buffer but zero vendor patches.
 *
 * The `rackboy_apu_t` struct owns the full SameBoy `GB_gameboy_t`
 * state plus scratch space for the 4-pass snapshot/restore cycle.
 * Callers allocate it (stack, static, or member); the driver makes
 * no heap allocations in the audio path.
 *
 * Thread model: audio-thread only. No internal locking.
 *
 * File reservation (beads rackboy-w7q): rackboy_apu/rackboy_apu.{h,c}
 */

#ifndef RACKBOY_APU_H
#define RACKBOY_APU_H

#include "rackboy_apu_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MPCBOY LOCAL MOD: register-write spy callback type (see rackboy_apu_set_write_spy). */
typedef void (*rackboy_apu_write_spy_fn)(void *user, uint16_t addr, uint8_t value);

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct rackboy_apu_s {
    /*
     * Live APU state. The `gb` field is first so that `(GB_gameboy_t *)apu`
     * yields the same address — the sample callback uses this to recover
     * the owning `rackboy_apu_t` pointer without an extra indirection.
     */
    GB_gameboy_t gb;

    /*
     * Scratch copy used by the 4-pass mute renderer. Snapshotted at the
     * start of `rackboy_apu_run_samples` and restored before each pass.
     *
     * On the 2nd+ call, the snapshot captures post-composite state from
     * the prior call — band_limited[active] holds that call's ring-down
     * energy. Per rackboy-6ne, the driver zeros band_limited[muted] at
     * pass start to prevent the prior call's filter residue from bleeding
     * into the current pass's output buffer.
     */
    GB_gameboy_t snapshot;

    /*
     * Per-channel band-limited filter saves. After each mute pass, the
     * unmuted channel's `band_limited` state is stashed here so the
     * composite final state preserves accurate filter history for all
     * four channels.
     */
    GB_band_limited_t bl_save[GB_N_CHANNELS];

    /* -- internal callback context (set per-pass, not caller-visible) -- */
    float   *_cb_buf;       /* current channel output buffer              */
    float   *_cb_buf_r;     /* MPCBOY LOCAL MOD: right buffer, run_stereo */
    uint32_t _cb_pos;       /* samples written so far this pass           */
    uint32_t _cb_target;    /* samples requested this pass                */

    /* -- configuration -- */
    unsigned sample_rate;   /* output sample rate (Hz), set at init       */

    /* MPCBOY LOCAL MOD: when non-zero, feed_samples also advances
     * gb.div_counter and fires GB_apu_div_event / GB_apu_div_secondary_event
     * (the 512 Hz frame sequencer: length counters, envelopes, sweep).
     * 0 (the default after init) keeps rackboy's original behaviour, in which
     * those events never fire. */
    uint8_t  mpcboy_frame_seq;

    /* MPCBOY LOCAL MOD: test-only register-write spy (NULL in production);
     * cleared by rackboy_apu_init. */
    rackboy_apu_write_spy_fn _spy;
    void                    *_spy_user;
} rackboy_apu_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Reset APU to DMG power-on state and configure the output sample rate.
 * Must be called before `write_reg` or `run_samples`.
 *
 * `sample_rate` is the host audio rate in Hz (e.g. 44100, 48000).
 * Passing 0 is undefined.
 */
void rackboy_apu_init(rackboy_apu_t *apu, unsigned sample_rate);

/*
 * Write a single APU register. `addr` is the CPU address (0xFF10 for
 * NR10, 0xFF26 for NR52, etc.); internally mapped to the SameBoy
 * io_registers[] offset. Writes outside the APU range (0xFF10..0xFF3F)
 * are silently ignored.
 *
 * Typical call sequence from the table interpreter:
 *
 *   rackboy_apu_write_reg(apu, 0xFF26, 0x80);   // NR52: APU on
 *   rackboy_apu_write_reg(apu, 0xFF25, 0xFF);   // NR51: all ch → L+R
 *   rackboy_apu_write_reg(apu, 0xFF24, 0x77);   // NR50: max volume
 *   rackboy_apu_write_reg(apu, 0xFF11, 0x80);   // NR11: 50% duty
 *   rackboy_apu_write_reg(apu, 0xFF12, 0xF0);   // NR12: vol 15, no env
 *   rackboy_apu_write_reg(apu, 0xFF13, 0xD6);   // NR13: freq low
 *   rackboy_apu_write_reg(apu, 0xFF14, 0x86);   // NR14: freq high + trigger
 */
void rackboy_apu_write_reg(rackboy_apu_t *apu, uint16_t addr, uint8_t value);

/*
 * Render `n` output samples into four caller-provided mono buffers
 * (one per Game Boy channel: CH1/Pulse1, CH2/Pulse2, CH3/Wave,
 * CH4/Noise). Each buffer must hold at least `n` floats.
 *
 * Output range is approximately [-1.0, +1.0] (normalized from the
 * SameBoy int16_t stereo output). The exact amplitude depends on the
 * channel's current volume envelope and NR50 master volume; passing
 * NULL for a channel buffer skips that channel's render pass.
 *
 * Internally performs 4 passes (D-009 mute rendering). The APU state
 * advances by exactly one buffer's worth of emulated time regardless
 * of how many channels are rendered.
 */
void rackboy_apu_run_samples(rackboy_apu_t *apu, uint32_t n,
                              float *ch1, float *ch2, float *ch3,
                              float *ch4);

/*
 * MPCBOY LOCAL MOD.
 *
 * Render `n` frames in ONE pass to a stereo pair (`left`, `right`), each
 * holding at least `n` floats. No channel muting, no snapshot, no band-limited
 * state swap: every channel is mixed through NR50/NR51 exactly as the register
 * values dictate, and the APU state simply carries over to the next call (so
 * rendering N frames in one call equals rendering them in any number of
 * smaller calls). Samples are SameBoy's int16 stereo output divided by 32768.
 *
 * Allocates nothing. Uses its own sample callback and restores rackboy's
 * callback afterwards, so `rackboy_apu_run_samples` keeps working.
 */
void rackboy_apu_run_stereo(rackboy_apu_t *apu, uint32_t n,
                            float *left, float *right);

/*
 * MPCBOY LOCAL MOD: enable or disable the frame sequencer (see the
 * `mpcboy_frame_seq` field). Upstream SameBoy drives it from the CPU's DIV
 * counter, which rackboy does not have, so without this no length counter,
 * volume envelope or frequency sweep ever advances. Call after
 * rackboy_apu_init (init clears it). Affects every render path (the flag is
 * read in feed_samples, which rackboy_apu_run_stereo and rackboy_apu_run_samples
 * both use); rackboy_apu_run_samples (the 4-pass golden path) must keep it off.
 */
void rackboy_apu_set_frame_sequencer(rackboy_apu_t *apu, int enable);

/*
 * MPCBOY LOCAL MOD: test-only register-write spy, one per APU instance.
 *
 * When `fn` is non-NULL it is called with (user, CPU address, value) for every
 * write that goes through `rackboy_apu_write_reg` on THIS apu and for every
 * write `rackboy_apu_write_wave_ram` performs (NR30 0, the 16 wave bytes, NR30
 * restore), immediately before the write reaches the APU. NULL (the default)
 * disables it; production code never sets it. The hook lives in the
 * rackboy_apu_t, so there is no process-global mutable state. Set it after
 * rackboy_apu_init (init clears it, as it clears the whole struct).
 */
void rackboy_apu_set_write_spy(rackboy_apu_t *apu, rackboy_apu_write_spy_fn fn,
                               void *user);

/*
 * Write all 16 bytes of CH3 wave RAM ($FF30..$FF3F) from the provided
 * 16-byte waveform buffer.
 *
 * SameBoy (and real Game Boy hardware) drops or mis-redirects writes to
 * $FF30..$FF3F while `is_active[GB_WAVE]` is true (DMG drops the write;
 * CGB E+ redirects to `current_sample_index / 2`). See
 * `vendor/sameboy/apu.c` around line 1696. This helper performs the
 * canonical safe write dance:
 *
 *   1. Snapshot NR30 (the DAC-enable register).
 *   2. Write 0x00 to NR30 (disables CH3's DAC → is_active[GB_WAVE]
 *      clears → subsequent wave-RAM writes land correctly).
 *   3. Write all 16 bytes in order to $FF30..$FF3F.
 *   4. Restore NR30 to its saved value (re-enables the DAC if it was on).
 *
 * APU-master-off edge case: when NR52 bit 7 is 0 (APU globally disabled),
 * SameBoy blocks NR30 writes (see the `!global_enable` guard at
 * `vendor/sameboy/apu.c:1685-1693`) so the snapshot/restore become
 * effective no-ops. Writes to registers below `GB_IO_WAV_START` are
 * gated, but wave-RAM writes themselves pass through — so the 16-byte
 * payload still lands in $FF30..$FF3F. Callers who need the DAC
 * re-enabled after an APU-off sequence must reset NR52 and re-configure
 * the channel themselves; this helper does not attempt to paper over
 * the global-enable state.
 *
 * The caller is responsible for re-triggering CH3 (NR34 bit 7) after
 * this helper if the wave-position pointer needs to reset to 0. Calling
 * this helper does NOT re-trigger the channel on its own.
 *
 * `waveform` must point to exactly 16 bytes. Passing NULL is undefined.
 *
 * Tracked by bead rackboy-9i0.
 */
void rackboy_apu_write_wave_ram(rackboy_apu_t *apu, const uint8_t waveform[16]);

#ifdef __cplusplus
}
#endif

#endif /* RACKBOY_APU_H */
