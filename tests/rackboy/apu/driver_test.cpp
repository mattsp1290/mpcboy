// tests/apu/driver_test.cpp
//
// Doctest suite for rackboy_apu (shim + taps + driver) — bead rackboy-2ej.
//
// Covers:
//   - shim struct sanity (offsets of critical fields)
//   - shim clock rate constant (DMG 2^22 Hz)
//   - init sets sample_rate + enables APU master
//   - register write round-trip (read back via io_registers[])
//   - silent stimulus → all-zero per-channel taps (single-channel render)
//   - Pulse1 / Noise stimulus → non-silent output on the triggered channel
//   - channel isolation: untriggered channels render bit-exact zero
//   - D-009 4-pass mute rendering: single call with all 4 buffers non-null
//     — see "D-009 4-pass mute rendering" section below (closes rackboy-gkv)
//
// Wave RAM API round-trip lives in tests/apu/wave_ram_test.cpp.

#include "doctest/doctest.h"

extern "C" {
#include "rackboy_apu.h"
}

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static rackboy_apu_t make_apu_at(unsigned sample_rate)
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, sample_rate);
    return apu;
}

// Build a freshly-initialized APU with Pulse1 triggered at a mid freq,
// max volume, 50% duty. Used by several stimulus tests.
static rackboy_apu_t make_pulse1_apu()
{
    rackboy_apu_t apu = make_apu_at(48000);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);  // NR50: master volume max
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);  // NR51: route all channels
    rackboy_apu_write_reg(&apu, 0xFF10, 0x00);  // NR10: no sweep
    rackboy_apu_write_reg(&apu, 0xFF11, 0xBF);  // NR11: duty 50% / max length
    rackboy_apu_write_reg(&apu, 0xFF12, 0xF0);  // NR12: vol 15
    rackboy_apu_write_reg(&apu, 0xFF13, 0xC1);  // NR13: freq lo
    rackboy_apu_write_reg(&apu, 0xFF14, 0x87);  // NR14: trigger + freq_hi=7
    return apu;
}

// Pulse2 triggered at the same freq/duty/volume as make_pulse1_apu().
// Writes NR21..NR24 instead of NR11..NR14.
static rackboy_apu_t make_pulse2_apu()
{
    rackboy_apu_t apu = make_apu_at(48000);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF16, 0xBF);  // NR21: duty 50% / max length
    rackboy_apu_write_reg(&apu, 0xFF17, 0xF0);  // NR22: vol 15
    rackboy_apu_write_reg(&apu, 0xFF18, 0xC1);  // NR23: freq lo
    rackboy_apu_write_reg(&apu, 0xFF19, 0x87);  // NR24: trigger + freq_hi
    return apu;
}

// CH3 Wave triggered with an alternating 0xFF / 0x00 wave pattern. NR30 DAC
// on, NR32 full output, NR33/NR34 mid freq.
static rackboy_apu_t make_wave_apu()
{
    rackboy_apu_t apu = make_apu_at(48000);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
    static const uint8_t wav[16] = {
        0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
        0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
    };
    rackboy_apu_write_wave_ram(&apu, wav);
    rackboy_apu_write_reg(&apu, 0xFF1A, 0x80);  // NR30: DAC on
    rackboy_apu_write_reg(&apu, 0xFF1B, 0x00);  // NR31: max length
    rackboy_apu_write_reg(&apu, 0xFF1C, 0x20);  // NR32: volume 100%
    rackboy_apu_write_reg(&apu, 0xFF1D, 0xC1);  // NR33: freq lo
    rackboy_apu_write_reg(&apu, 0xFF1E, 0x87);  // NR34: trigger + freq_hi
    return apu;
}

// CH4 Noise triggered. Shared by the single-channel and 4-pass cases below.
static rackboy_apu_t make_noise_apu()
{
    rackboy_apu_t apu = make_apu_at(48000);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF20, 0x3F);  // NR41 length
    rackboy_apu_write_reg(&apu, 0xFF21, 0xF0);  // NR42: vol 15
    rackboy_apu_write_reg(&apu, 0xFF22, 0x37);  // NR43: clock/shape
    rackboy_apu_write_reg(&apu, 0xFF23, 0x80);  // NR44: trigger
    return apu;
}

// ---------------------------------------------------------------------------
// Shim struct sanity
// ---------------------------------------------------------------------------

TEST_CASE("rackboy_apu_t: gb is the first member (callback cast contract)")
{
    // The sample callback casts GB_gameboy_t* to rackboy_apu_t* via the
    // first-member trick. Static asserts in rackboy_apu.c enforce this at
    // build time; the test is belt-and-suspenders.
    CHECK_EQ(offsetof(rackboy_apu_t, gb), 0u);
}

TEST_CASE("rackboy_apu_t: snapshot field follows gb (sized for swap-restore)")
{
    // The 4-pass mute renderer memcpys apu->snapshot ← apu->gb before
    // the first pass, then restores before subsequent passes. Both
    // fields must be GB_gameboy_t.
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    CHECK_EQ(sizeof(apu.gb), sizeof(apu.snapshot));
}

TEST_CASE("shim GB_get_clock_rate returns DMG master clock 4194304 Hz")
{
    // The shim stub pins the Game Boy master clock at 2^22 Hz (the DMG
    // rate used by the feed_samples timing model). If SameBoy or the
    // shim ever changes this, feed_samples's deficit math in
    // rackboy_apu.c would skew and audio output would land at the wrong
    // sample rate. Asserted here since the bead description lists
    // "clock rate" as a distinct coverage bullet.
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    CHECK_EQ(GB_get_clock_rate(&apu.gb), 4194304u);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

TEST_CASE("rackboy_apu_init: sample_rate is stored")
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    CHECK_EQ(apu.sample_rate, 48000u);
}

TEST_CASE("rackboy_apu_init: different sample rates stored verbatim")
{
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) {
        rackboy_apu_t apu;
        std::memset(&apu, 0, sizeof(apu));
        rackboy_apu_init(&apu, rate);
        INFO("rate = ", rate);
        CHECK_EQ(apu.sample_rate, rate);
    }
}

TEST_CASE("rackboy_apu_init: NR52 master enable is set (bit 7)")
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);

    // init writes 0x80 to NR52 to enable the APU master switch.
    // Without this, writes to NR10..NR51 are silently ignored.
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR52] & 0x80, 0x80);
}

// ---------------------------------------------------------------------------
// Register write round-trips
// ---------------------------------------------------------------------------

TEST_CASE("rackboy_apu_write_reg: writes land in io_registers[]")
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);

    struct { uint16_t addr; uint8_t val; uint8_t idx; } cases[] = {
        // Non-trigger registers that read back the written value cleanly.
        { 0xFF10, 0x56, GB_IO_NR10 },   // sweep
        { 0xFF11, 0x80, GB_IO_NR11 },   // duty/length
        { 0xFF13, 0xD6, GB_IO_NR13 },   // CH1 freq lo
        { 0xFF17, 0xA0, GB_IO_NR22 },   // CH2 envelope
        { 0xFF22, 0x37, GB_IO_NR43 },   // CH4 shape
        { 0xFF1A, 0x80, GB_IO_NR30 },   // CH3 DAC
    };

    for (auto& c : cases) {
        rackboy_apu_write_reg(&apu, c.addr, c.val);
        INFO("addr = ", c.addr);
        CHECK_EQ(apu.gb.io_registers[c.idx], c.val);
    }
}

TEST_CASE("rackboy_apu_write_reg: writes outside APU range are ignored")
{
    rackboy_apu_t apu = make_apu_at(48000);

    // Snapshot the target addresses so we can assert they don't change.
    const uint8_t ff00_before = apu.gb.io_registers[0x00];
    const uint8_t ff40_before = apu.gb.io_registers[0x40];
    const uint8_t nr10_before = apu.gb.io_registers[GB_IO_NR10];

    // 0x00..0x0F and 0x40..0xFF are outside the APU range.
    rackboy_apu_write_reg(&apu, 0xFF00, 0xFF);  // below range
    rackboy_apu_write_reg(&apu, 0xFF40, 0xFF);  // above range

    CHECK_EQ(apu.gb.io_registers[0x00], ff00_before);
    CHECK_EQ(apu.gb.io_registers[0x40], ff40_before);
    CHECK_EQ(apu.gb.io_registers[GB_IO_NR10], nr10_before);
}

TEST_CASE("rackboy_apu_write_reg: range boundaries (0xFF0F / 0xFF3F)")
{
    // Pin both sides of the range check in rackboy_apu.c:146.
    // 0xFF0F is one below the APU range → ignored.
    // 0xFF3F is the last APU byte (wave RAM) → accepted.
    rackboy_apu_t apu = make_apu_at(48000);

    const uint8_t ff0f_before = apu.gb.io_registers[0x0F];
    const uint8_t ff3f_before = apu.gb.io_registers[GB_IO_WAV_END];

    rackboy_apu_write_reg(&apu, 0xFF0F, 0xAA);  // below: ignored
    CHECK_EQ(apu.gb.io_registers[0x0F], ff0f_before);

    // 0xFF3F: top of range, accepted. Use the wave RAM helper to avoid
    // is_active[GB_WAVE] gating — or just write directly since CH3 DAC
    // starts off. We verify a direct byte write lands.
    rackboy_apu_write_reg(&apu, 0xFF3F, 0x55);
    CHECK_NE(apu.gb.io_registers[GB_IO_WAV_END], ff3f_before);
    CHECK_EQ(apu.gb.io_registers[GB_IO_WAV_END], 0x55);
}

// ---------------------------------------------------------------------------
// Silent stimulus — no triggered channels → all taps silent
// ---------------------------------------------------------------------------

TEST_CASE("rackboy_apu_run_samples: silent stimulus yields all-zero taps")
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);

    // No channel triggers → the APU should render silence. Render one
    // channel at a time (single-channel runs are the shape wave_ram_test
    // and the production audio path use).
    const uint32_t N = 64;
    float ch[N];

    for (int which = 0; which < 4; ++which) {
        std::memset(ch, 0, sizeof(ch));
        float *bufs[4] = { nullptr, nullptr, nullptr, nullptr };
        bufs[which] = ch;

        // Fresh APU per sub-test so state from a prior render doesn't leak.
        rackboy_apu_t apu_fresh;
        std::memset(&apu_fresh, 0, sizeof(apu_fresh));
        rackboy_apu_init(&apu_fresh, 48000);

        rackboy_apu_run_samples(&apu_fresh, N,
                                bufs[0], bufs[1], bufs[2], bufs[3]);

        for (uint32_t i = 0; i < N; ++i) {
            INFO("which = ", which, ", i = ", i);
            CHECK_EQ(ch[i], 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Pulse1 stimulus → CH1 non-silent, CH2/CH3/CH4 bit-exact-zero
// ---------------------------------------------------------------------------
//
// This pins the channel-isolation contract of D-009's 4-pass mute
// rendering: when only Pulse1 is triggered, the other three channels
// must produce bit-exact 0.0f samples (SameBoy's per-channel mute
// mechanism routes the muted channel's contribution away from the
// output). If this test fails, the renderer has lost channel isolation.

TEST_CASE("Pulse1 stimulus: CH1 renders non-silent output")
{
    rackboy_apu_t apu = make_pulse1_apu();

    const uint32_t N = 256;
    float ch1[N] = {0};
    rackboy_apu_run_samples(&apu, N, ch1, nullptr, nullptr, nullptr);

    // Count non-zero samples. A single-impulse regression would still
    // fail because a real Pulse1 square wave produces many non-zero
    // samples per buffer.
    uint32_t nonzero_count = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch1[i] != 0.0f) ++nonzero_count;
    }
    // At 48 kHz rendering ~1.4 kHz square wave, roughly half the samples
    // should be non-zero. Assert generously (at least 10% of the buffer)
    // so future band-limiting tweaks don't spuriously fail.
    CHECK_GT(nonzero_count, N / 10);
}

TEST_CASE("Pulse1 stimulus: untriggered channels render silent (single-buffer path)")
{
    // Channel isolation check via single-channel renders on fresh APUs.
    // Trigger only Pulse1, then render each OTHER channel separately —
    // each must produce bit-exact zero because nothing else is triggered.
    //
    // The companion all-4-buffers coverage lives in the D-009 4-pass
    // render section below; this case still pins the single-buffer path.

    const uint32_t N = 128;

    // CH2 (Pulse2): untriggered, must be silent.
    {
        rackboy_apu_t apu = make_pulse1_apu();
        float ch[N] = {0};
        rackboy_apu_run_samples(&apu, N, nullptr, ch, nullptr, nullptr);
        for (uint32_t i = 0; i < N; ++i) {
            INFO("CH2 sample ", i);
            CHECK_EQ(ch[i], 0.0f);
        }
    }

    // CH3 (Wave): untriggered, NR30 DAC off by default, must be silent.
    {
        rackboy_apu_t apu = make_pulse1_apu();
        float ch[N] = {0};
        rackboy_apu_run_samples(&apu, N, nullptr, nullptr, ch, nullptr);
        for (uint32_t i = 0; i < N; ++i) {
            INFO("CH3 sample ", i);
            CHECK_EQ(ch[i], 0.0f);
        }
    }

    // CH4 (Noise): untriggered, must be silent.
    {
        rackboy_apu_t apu = make_pulse1_apu();
        float ch[N] = {0};
        rackboy_apu_run_samples(&apu, N, nullptr, nullptr, nullptr, ch);
        for (uint32_t i = 0; i < N; ++i) {
            INFO("CH4 sample ", i);
            CHECK_EQ(ch[i], 0.0f);
        }
    }
}

TEST_CASE("Noise stimulus: CH4 renders non-silent output")
{
    rackboy_apu_t apu = make_noise_apu();

    const uint32_t N = 256;
    float ch4[N] = {0};
    rackboy_apu_run_samples(&apu, N, nullptr, nullptr, nullptr, ch4);

    // Noise channel should produce many non-zero samples per buffer at
    // this clock rate. Assert at least 10% non-zero so a single-impulse
    // regression still fails.
    uint32_t nonzero_count = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch4[i] != 0.0f) ++nonzero_count;
    }
    CHECK_GT(nonzero_count, N / 10);
}

// ---------------------------------------------------------------------------
// run_samples with NULL buffers advances timeline without rendering
// ---------------------------------------------------------------------------

TEST_CASE("rackboy_apu_run_samples: n=0 is a no-op")
{
    rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);

    float ch1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    rackboy_apu_run_samples(&apu, 0, ch1, nullptr, nullptr, nullptr);

    // Buffer must be untouched.
    CHECK_EQ(ch1[0], 1.0f);
    CHECK_EQ(ch1[1], 2.0f);
    CHECK_EQ(ch1[2], 3.0f);
    CHECK_EQ(ch1[3], 4.0f);
}

// ---------------------------------------------------------------------------
// D-009 4-pass mute rendering — single-call path with all 4 buffers non-null
// ---------------------------------------------------------------------------
//
// These cases exercise `rackboy_apu_run_samples` with all four channel
// buffers non-null in a single call. This is the original channel-isolation
// check deferred by bead rackboy-gkv (see iteration 51). The bug was filed
// as "hangs indefinitely at 98% CPU inside feed_samples" but is no longer
// reproducible on main — these cases re-enable the direct coverage.

TEST_CASE("4-pass render: silent stimulus yields all-zero across all 4 channels")
{
    rackboy_apu_t apu = make_apu_at(48000);

    const uint32_t N = 64;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    for (uint32_t i = 0; i < N; ++i) {
        INFO("i=", i);
        CHECK_EQ(ch1[i], 0.0f);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
}

TEST_CASE("4-pass render: Pulse1 triggered → CH1 audible, CH2/3/4 bit-exact zero")
{
    rackboy_apu_t apu = make_pulse1_apu();

    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nonzero_ch1 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch1[i] != 0.0f) ++nonzero_ch1;
        INFO("i=", i);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
    CHECK_GT(nonzero_ch1, N / 10);
}

TEST_CASE("4-pass render: Noise triggered → CH4 audible, CH1/2/3 bit-exact zero")
{
    rackboy_apu_t apu = make_noise_apu();

    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nonzero_ch4 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch4[i] != 0.0f) ++nonzero_ch4;
        INFO("i=", i);
        CHECK_EQ(ch1[i], 0.0f);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
    }
    CHECK_GT(nonzero_ch4, N / 10);
}

TEST_CASE("4-pass render: Pulse2 triggered → CH2 audible, CH1/3/4 bit-exact zero")
{
    rackboy_apu_t apu = make_pulse2_apu();
    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nonzero_ch2 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch2[i] != 0.0f) ++nonzero_ch2;
        INFO("i=", i);
        CHECK_EQ(ch1[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
    CHECK_GT(nonzero_ch2, N / 10);
}

TEST_CASE("4-pass render: Wave CH3 triggered → CH3 audible, CH1/2/4 bit-exact zero")
{
    // CH3 is the highest-risk mute-render target: NR30 DAC gate and the
    // is_active[GB_WAVE] interlock in the vendored SameBoy APU only get
    // exercised when CH3 is the unmuted pass.
    rackboy_apu_t apu = make_wave_apu();
    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nonzero_ch3 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch3[i] != 0.0f) ++nonzero_ch3;
        INFO("i=", i);
        CHECK_EQ(ch1[i], 0.0f);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
    CHECK_GT(nonzero_ch3, N / 10);
}

TEST_CASE("4-pass render: Pulse1 + Noise both triggered → CH1 + CH4 audible, CH2/3 silent")
{
    // Simultaneous multi-channel trigger is the first case that actually
    // exercises the composite bl_save[ch] stitch-back loop in rackboy_apu.c.
    // With ≤1 active channel the composite logic is trivially correct.
    rackboy_apu_t apu = make_pulse1_apu();
    rackboy_apu_write_reg(&apu, 0xFF20, 0x3F);  // NR41
    rackboy_apu_write_reg(&apu, 0xFF21, 0xF0);  // NR42
    rackboy_apu_write_reg(&apu, 0xFF22, 0x37);  // NR43
    rackboy_apu_write_reg(&apu, 0xFF23, 0x80);  // NR44 trigger

    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nz1 = 0, nz4 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch1[i] != 0.0f) ++nz1;
        if (ch4[i] != 0.0f) ++nz4;
        INFO("i=", i);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
    }
    CHECK_GT(nz1, N / 10);
    CHECK_GT(nz4, N / 10);
}

TEST_CASE("4-pass render: 44.1 kHz sample rate exercises non-48 kHz timing path")
{
    // feed_samples() in rackboy_apu.c has sample-rate-dependent arithmetic
    // (ceiling deficit math, uint16_t apu_cyc downcast). The original
    // rackboy-gkv symptom pointed into feed_samples, so pin a second rate.
    rackboy_apu_t apu = make_apu_at(44100);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF10, 0x00);
    rackboy_apu_write_reg(&apu, 0xFF11, 0xBF);
    rackboy_apu_write_reg(&apu, 0xFF12, 0xF0);
    rackboy_apu_write_reg(&apu, 0xFF13, 0xC1);
    rackboy_apu_write_reg(&apu, 0xFF14, 0x87);

    const uint32_t N = 256;
    float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};

    rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

    uint32_t nz1 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch1[i] != 0.0f) ++nz1;
        INFO("i=", i);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
    CHECK_GT(nz1, N / 10);
}

TEST_CASE("4-pass render: two back-to-back calls keep channel isolation bit-exact")
{
    // apu->snapshot is re-captured on each run_samples call, so the
    // post-composite state of call N becomes the snapshot input of call
    // N+1. rackboy-6ne: without the per-pass band_limited[muted] clear
    // in rackboy_apu.c, band_limited[active] from call N would ring down
    // through call N+1's muted passes and bleed into the other channels.
    // This case guards the fix — any regression that drops the clear
    // will show up as non-zero ch2/ch3/ch4 on round 1.
    rackboy_apu_t apu = make_pulse1_apu();
    const uint32_t N = 256;

    for (int round = 0; round < 2; ++round) {
        float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};
        rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

        uint32_t nz1 = 0;
        for (uint32_t i = 0; i < N; ++i) {
            if (ch1[i] != 0.0f) ++nz1;
            INFO("round=", round, " i=", i);
            CHECK_EQ(ch2[i], 0.0f);
            CHECK_EQ(ch3[i], 0.0f);
            CHECK_EQ(ch4[i], 0.0f);
        }
        CHECK_GT(nz1, N / 10);
    }
}

TEST_CASE("4-pass render: two back-to-back calls with two active channels keep isolation")
{
    // Companion to the single-active-channel back-to-back case above.
    // Pulse1 + Noise triggered together exercises the composite stitch-back
    // loop in rackboy_apu.c:300-315 in combination with the per-pass
    // band_limited[muted] clear from rackboy-6ne. A regression that drops
    // the clear, or that stitches bl_save into the wrong channel index,
    // would show up as non-zero ch2/ch3 on round 1.
    rackboy_apu_t apu = make_pulse1_apu();
    rackboy_apu_write_reg(&apu, 0xFF20, 0x3F);  // NR41
    rackboy_apu_write_reg(&apu, 0xFF21, 0xF0);  // NR42
    rackboy_apu_write_reg(&apu, 0xFF22, 0x37);  // NR43
    rackboy_apu_write_reg(&apu, 0xFF23, 0x80);  // NR44 trigger

    const uint32_t N = 256;

    for (int round = 0; round < 2; ++round) {
        float ch1[N] = {0}, ch2[N] = {0}, ch3[N] = {0}, ch4[N] = {0};
        rackboy_apu_run_samples(&apu, N, ch1, ch2, ch3, ch4);

        uint32_t nz1 = 0, nz4 = 0;
        for (uint32_t i = 0; i < N; ++i) {
            if (ch1[i] != 0.0f) ++nz1;
            if (ch4[i] != 0.0f) ++nz4;
            INFO("round=", round, " i=", i);
            CHECK_EQ(ch2[i], 0.0f);
            CHECK_EQ(ch3[i], 0.0f);
        }
        CHECK_GT(nz1, N / 10);
        CHECK_GT(nz4, N / 10);
    }
}

TEST_CASE("4-pass render: larger N (2048 samples) maintains channel isolation")
{
    // All other 4-pass cases use N=64 or N=256. A regression that only
    // manifests after many internal renders per call would be missed
    // without exercising a production-scale buffer size.
    rackboy_apu_t apu = make_pulse1_apu();
    const uint32_t N = 2048;
    std::vector<float> ch1(N, 0.0f), ch2(N, 0.0f), ch3(N, 0.0f), ch4(N, 0.0f);

    rackboy_apu_run_samples(&apu, N, ch1.data(), ch2.data(), ch3.data(), ch4.data());

    uint32_t nz1 = 0;
    for (uint32_t i = 0; i < N; ++i) {
        if (ch1[i] != 0.0f) ++nz1;
        INFO("i=", i);
        CHECK_EQ(ch2[i], 0.0f);
        CHECK_EQ(ch3[i], 0.0f);
        CHECK_EQ(ch4[i], 0.0f);
    }
    CHECK_GT(nz1, N / 10);
}
