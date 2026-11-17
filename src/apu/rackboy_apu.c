/*
 * rackboy_apu/rackboy_apu.c
 *
 * Implementation of the rackboy APU driver — see rackboy_apu.h for the
 * public API contract and D-009 for the 4-pass mute rendering design.
 *
 * Timing model
 * ------------
 * SameBoy's APU is driven by the CPU timing loop in Core/timing.c.
 * Each iteration of that loop advances 4 T-cycles of the master clock
 * (4,194,304 Hz for DMG) and does:
 *
 *   apu.apu_cycles    += 2            (APU runs at 2 MHz = master/2)
 *   apu_output.sample_cycles += sample_rate * 4
 *
 * A render (sample callback) fires when sample_cycles >= clock_rate.
 * Since rackboy has no CPU, this file replicates the timing by
 * manually incrementing both counters before each GB_apu_run call.
 *
 * File reservation (beads rackboy-w7q): rackboy_apu/rackboy_apu.{h,c}
 */

#include "rackboy_apu.h"

#include <stddef.h>
#include <string.h>

/*
 * The sample callback recovers rackboy_apu_t* from the GB_gameboy_t*
 * argument by a straight cast. This is well-defined in C only when
 * `gb` is the first member (no padding before it).
 */
_Static_assert(offsetof(rackboy_apu_t, gb) == 0,
    "rackboy_apu_t.gb must be the first member for the callback cast");

/* DMG master clock rate (2^22 Hz). */
#define RACKBOY_CLOCK_RATE 4194304u

/* ------------------------------------------------------------------ */
/* Sample callback                                                     */
/* ------------------------------------------------------------------ */

/*
 * Called by SameBoy's render() inside GB_apu_run whenever enough
 * emulated cycles have elapsed to produce one output sample at the
 * configured sample rate.
 *
 * During a mute pass only one channel is unmuted, and NR50/NR51 are
 * overridden so the stereo output is that channel routed to both L
 * and R at max master volume. We take the left sample as the mono
 * value and normalize to float.
 */
static void rackboy_sample_callback(GB_gameboy_t *gb, GB_sample_t *sample)
{
    /* Recover the owning rackboy_apu_t — gb is its first member. */
    rackboy_apu_t *apu = (rackboy_apu_t *)gb;

    if (apu->_cb_pos < apu->_cb_target && apu->_cb_buf) {
        apu->_cb_buf[apu->_cb_pos++] = (float)sample->left / 32768.0f;
    }
}

/*
 * MPCBOY LOCAL MOD: single-pass stereo callback. Stores both sides; used only
 * by rackboy_apu_run_stereo, which swaps it in and restores
 * rackboy_sample_callback afterwards.
 */
static void mpcboy_stereo_callback(GB_gameboy_t *gb, GB_sample_t *sample)
{
    rackboy_apu_t *apu = (rackboy_apu_t *)gb;

    if (apu->_cb_pos < apu->_cb_target && apu->_cb_buf && apu->_cb_buf_r) {
        apu->_cb_buf[apu->_cb_pos]   = (float)sample->left  / 32768.0f;
        apu->_cb_buf_r[apu->_cb_pos] = (float)sample->right / 32768.0f;
        apu->_cb_pos++;
    }
}

/* MPCBOY LOCAL MOD: test-only register-write spy (NULL in production), per APU. */
void rackboy_apu_set_write_spy(rackboy_apu_t *apu, rackboy_apu_write_spy_fn fn,
                               void *user)
{
    apu->_spy      = fn;
    apu->_spy_user = user;
}

static inline void apu_write_spied(rackboy_apu_t *apu, uint8_t reg, uint8_t value)
{
    if (apu->_spy) apu->_spy(apu->_spy_user, (uint16_t)(0xFF00u | reg), value);
    GB_apu_write(&apu->gb, reg, value);
}

/* ------------------------------------------------------------------ */
/* Cycle feeder                                                        */
/* ------------------------------------------------------------------ */

/*
 * MPCBOY LOCAL MOD: advance both counters by `t_cycles` (even) T-cycles. With
 * the frame sequencer off this is exactly rackboy's original sequence (one
 * GB_apu_run for the whole chunk). With it on, the chunk is split at every
 * 4096-cycle edge of DIV bit 12 and, like SameBoy's
 * GB_set_internal_div_counter, a falling edge fires GB_apu_div_event and a
 * rising edge GB_apu_div_secondary_event.
 */
static void advance_cycles(rackboy_apu_t *apu, uint32_t t_cycles)
{
    const unsigned sr = apu->sample_rate;

    while (t_cycles) {
        uint32_t step = t_cycles;
        if (apu->mpcboy_frame_seq) {
            const uint32_t to_edge = 4096u - ((uint32_t)apu->gb.div_counter & 4095u);
            if (step > to_edge) step = to_edge;
        }

        apu->gb.apu_output.sample_cycles += (uint32_t)((uint64_t)sr * step);
        apu->gb.apu.apu_cycles = (uint16_t)(step / 2);
        GB_apu_run(&apu->gb, true);
        t_cycles -= step;

        if (apu->mpcboy_frame_seq) {
            const uint16_t old_div = apu->gb.div_counter;
            const uint16_t new_div = (uint16_t)(old_div + step);
            apu->gb.div_counter = new_div;
            if ((uint16_t)(old_div & ~new_div) & 0x1000) {
                GB_apu_div_event(&apu->gb);
            }
            else if ((uint16_t)(~old_div & new_div) & 0x1000) {
                GB_apu_div_secondary_event(&apu->gb);
            }
        }
    }
}

/*
 * Drive the APU for exactly `n` output samples by replicating SameBoy's
 * timing.c per-T-cycle accounting.
 *
 * For each sample we calculate how many more T-cycles are needed for
 * sample_cycles to reach clock_rate, feed those cycles (incrementing
 * both apu_cycles and sample_cycles), and let GB_apu_run's internal
 * splitting call render() at the correct moment.
 */
static void feed_samples(rackboy_apu_t *apu, uint32_t n)
{
    const uint32_t cr = RACKBOY_CLOCK_RATE;
    const unsigned  sr = apu->sample_rate;

    while (apu->_cb_pos < n) {
        /*
         * How many T-cycles until sample_cycles crosses clock_rate?
         *
         * sample_cycles increments by (sr * t) for t T-cycles.
         * We need: current_sc + sr * t >= cr
         *      =>  t >= (cr - current_sc) / sr           (ceiling)
         *
         * If sample_cycles is already >= cr (residual from a previous
         * buffer boundary), feeding even 2 T-cycles will trigger a
         * render immediately.
         */
        uint32_t sc = apu->gb.apu_output.sample_cycles;
        uint32_t t_cycles;

        if (sc >= cr) {
            /* Render is already pending — minimal nudge to flush it. */
            t_cycles = 2;
        } else {
            uint32_t deficit = cr - sc;
            t_cycles = (deficit + sr - 1) / sr;       /* ceiling       */
            if (t_cycles < 2) t_cycles = 2;           /* minimum pulse */
        }

        /* Round up to even so apu_cycles (= t/2) is integral. */
        t_cycles = (t_cycles + 1) & ~1u;

        /* Replicate timing.c: advance both counters in lockstep. */
        advance_cycles(apu, t_cycles);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void rackboy_apu_init(rackboy_apu_t *apu, unsigned sample_rate)
{
    memset(apu, 0, sizeof(*apu));   /* also clears the (MPCBOY LOCAL MOD) write spy */

    apu->sample_rate = sample_rate;
    apu->gb.model    = GB_MODEL_DMG_B;

    GB_apu_init(&apu->gb);
    GB_set_sample_rate(&apu->gb, sample_rate);
    GB_set_highpass_filter_mode(&apu->gb, GB_HIGHPASS_OFF);
    GB_set_interference_volume(&apu->gb, 0.0);
    GB_apu_set_sample_callback(&apu->gb, rackboy_sample_callback);

    /*
     * Enable the APU master switch (NR52 bit 7). Until this is set,
     * SameBoy ignores writes to NR10..NR51.
     */
    GB_apu_write(&apu->gb, GB_IO_NR52, 0x80);
}

void rackboy_apu_set_frame_sequencer(rackboy_apu_t *apu, int enable)
{
    apu->mpcboy_frame_seq = enable ? 1 : 0;
}

void rackboy_apu_write_reg(rackboy_apu_t *apu, uint16_t addr, uint8_t value)
{
    uint8_t reg = (uint8_t)(addr & 0xFF);

    /* APU registers live at 0xFF10..0xFF3F (IO offsets 0x10..0x3F). */
    if (reg < 0x10 || reg > 0x3F) return;

    apu_write_spied(apu, reg, value);
}

/*
 * Pin the wave-RAM IO range so the 16-iteration loop below is obviously
 * safe. If the shim's constant drifts, this halts the build instead of
 * silently writing past the wave RAM.
 */
_Static_assert(GB_IO_WAV_END - GB_IO_WAV_START == 15,
               "CH3 wave RAM must be exactly 16 bytes ($FF30..$FF3F)");

void rackboy_apu_write_wave_ram(rackboy_apu_t *apu, const uint8_t waveform[16])
{
    /*
     * SameBoy drops or mis-redirects writes to $FF30..$FF3F while
     * `is_active[GB_WAVE]` is true (see vendor/sameboy/apu.c:1696).
     * Disable the DAC, write the 16 bytes, then restore NR30 so the
     * caller's CH3 configuration is preserved. Full semantics — including
     * the APU-master-off edge case — are documented in rackboy_apu.h.
     */
    const uint8_t saved_nr30 = apu->gb.io_registers[GB_IO_NR30];

    apu_write_spied(apu, GB_IO_NR30, 0x00);

    for (uint8_t i = 0; i < 16; i++) {
        apu_write_spied(apu, (uint8_t)(GB_IO_WAV_START + i), waveform[i]);
    }

    apu_write_spied(apu, GB_IO_NR30, saved_nr30);
}

void rackboy_apu_run_stereo(rackboy_apu_t *apu, uint32_t n,
                            float *left, float *right)
{
    if (n == 0) return;

    apu->_cb_buf    = left;
    apu->_cb_buf_r  = right;
    apu->_cb_pos    = 0;
    apu->_cb_target = n;
    apu->gb.apu_output.sample_callback = mpcboy_stereo_callback;

    feed_samples(apu, n);

    apu->gb.apu_output.sample_callback = rackboy_sample_callback;
    apu->_cb_buf   = NULL;
    apu->_cb_buf_r = NULL;
}

void rackboy_apu_run_samples(rackboy_apu_t *apu, uint32_t n,
                              float *ch1, float *ch2, float *ch3,
                              float *ch4)
{
    if (n == 0) return;

    float *bufs[GB_N_CHANNELS] = { ch1, ch2, ch3, ch4 };

    /* Count how many channels are actually requested. */
    int passes_needed = 0;
    for (int i = 0; i < GB_N_CHANNELS; i++) {
        if (bufs[i]) passes_needed++;
    }

    /*
     * If no output buffers were provided, still advance the APU state
     * by one buffer's worth of time so the caller's timeline stays
     * consistent.
     */
    if (passes_needed == 0) {
        apu->_cb_buf    = NULL;
        apu->_cb_pos    = 0;
        apu->_cb_target = n;
        feed_samples(apu, n);
        apu->_cb_buf = NULL;
        return;
    }

    /* -------------------------------------------------------------- */
    /* Snapshot the live state before the 4-pass mute cycle.           */
    /* -------------------------------------------------------------- */
    memcpy(&apu->snapshot, &apu->gb, sizeof(GB_gameboy_t));

    int last_rendered_ch = -1;

    for (int ch = 0; ch < GB_N_CHANNELS; ch++) {
        if (!bufs[ch]) continue;  /* caller doesn't want this channel */

        /* Restore from snapshot (first pass skips — gb IS the snapshot). */
        if (last_rendered_ch >= 0) {
            memcpy(&apu->gb, &apu->snapshot, sizeof(GB_gameboy_t));
        }

        /* Configure muting: only channel `ch` is unmuted. */
        for (int j = 0; j < GB_N_CHANNELS; j++) {
            apu->gb.apu_output.channel_muted[j] = (j != ch);
        }

        /*
         * Zero the band-limited filter state for every muted channel so
         * render() starts this pass with clean output history for the three
         * channels that must produce bit-exact zero samples. Without this
         * clear, on the second+ call to rackboy_apu_run_samples, the snapshot
         * captures the previous call's post-composite band_limited[active] —
         * non-zero filter energy that rings down for several samples under
         * the muted pass and bleeds into the unmuted channel's output buffer.
         * Discovered and tracked as bead rackboy-6ne.
         *
         * Only the muted channels' filters need clearing — the unmuted
         * channel's filter carries the correct continuity from the snapshot.
         */
        for (int j = 0; j < GB_N_CHANNELS; j++) {
            if (j != ch) {
                memset(&apu->gb.apu_output.band_limited[j], 0,
                       sizeof(GB_band_limited_t));
            }
        }

        /*
         * Override NR50/NR51 for a clean, known-scale per-channel output.
         *
         * NR50 = 0x77 → master volume 7 (max) on both L and R.
         * NR51 = route only this channel to both L and R.
         *
         * Using GB_apu_write (rather than a raw io_registers[] poke)
         * triggers SameBoy's immediate update_sample() refresh for all
         * channels, so the band-limited buffers reflect the new
         * routing/gain from the very first sample of this pass. The
         * internal GB_apu_run flush inside GB_apu_write is a no-op here
         * because apu_cycles is 0 after the snapshot restore.
         */
        GB_apu_write(&apu->gb, GB_IO_NR50, 0x77);
        GB_apu_write(&apu->gb, GB_IO_NR51,
                     (uint8_t)((1u << ch) | (0x10u << ch)));

        /* Wire up callback context for this pass. */
        apu->_cb_buf    = bufs[ch];
        apu->_cb_pos    = 0;
        apu->_cb_target = n;

        /* Ensure the callback pointer survived the memcpy restore. */
        apu->gb.apu_output.sample_callback = rackboy_sample_callback;

        /* Drive the APU for n output samples. */
        feed_samples(apu, n);

        /*
         * Stash this pass's band-limited filter state for channel `ch`.
         * Each channel's filter tracks the recent output history of that
         * channel; the muted channels' filters received zeros during
         * this pass and diverged from the "all unmuted" trajectory. We
         * save the unmuted channel's filter here and composite it back
         * after all passes.
         */
        memcpy(&apu->bl_save[ch],
               &apu->gb.apu_output.band_limited[ch],
               sizeof(GB_band_limited_t));

        last_rendered_ch = ch;
    }

    /* -------------------------------------------------------------- */
    /* Composite the canonical post-buffer state.                      */
    /*                                                                 */
    /* Core APU state (apu->gb.apu) is identical across all passes —   */
    /* channel state machines tick regardless of muting. Keep the last */
    /* pass's state, then overwrite each channel's band-limited filter */
    /* from the pass where that channel was unmuted.                    */
    /* -------------------------------------------------------------- */
    for (int ch = 0; ch < GB_N_CHANNELS; ch++) {
        if (!bufs[ch]) {
            /*
             * Channel was never rendered this call. Its band-limited
             * state in the current gb is from the last pass (where it
             * was muted). That's the best we have — it still advanced
             * by the correct number of cycles, just with zero input.
             * This is acceptable because a NULL buffer means the caller
             * doesn't care about this channel's output fidelity.
             */
            continue;
        }
        memcpy(&apu->gb.apu_output.band_limited[ch],
               &apu->bl_save[ch],
               sizeof(GB_band_limited_t));
    }

    /* Restore original NR50/NR51 so the live state reflects the real
     * register values the table interpreter wrote. Using GB_apu_write
     * (not a raw io_registers[] poke) triggers the update_sample()
     * refresh, keeping the band-limited filter state consistent with
     * the restored register values. */
    GB_apu_write(&apu->gb, GB_IO_NR50,
                 apu->snapshot.io_registers[GB_IO_NR50]);
    GB_apu_write(&apu->gb, GB_IO_NR51,
                 apu->snapshot.io_registers[GB_IO_NR51]);

    /* Clear all mute flags — the live state should appear unmuted. */
    for (int j = 0; j < GB_N_CHANNELS; j++) {
        apu->gb.apu_output.channel_muted[j] = false;
    }

    /* Defense-in-depth: clear the callback buffer pointer so a stray
     * render() call outside rackboy_apu_run_samples cannot write
     * into a caller buffer that may no longer be valid. */
    apu->_cb_buf = NULL;
}
