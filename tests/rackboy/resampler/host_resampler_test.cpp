// tests/resampler/host_resampler_test.cpp
//
// Doctest coverage for the streaming HostResampler (bead
// rackboy-s20). Exercises the production integration primitive
// that pins the APU at 48 kHz and fractionally resamples to each
// of the four supported host rates per docs/DECISIONS.md D-012.

#include "doctest/doctest.h"

#include "measurement.hpp"
#include "rackboy_resampler.hpp"
#include "spike/vcv_resampler/fft.h"

extern "C" {
#include "rackboy_apu.h"
}

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using rackboy::resampler::HostResampler;
using rackboy::resampler::host_resampler_emit;
using rackboy::resampler::host_resampler_init;
using rackboy::resampler::host_resampler_push_input;
using rackboy::resampler::host_resampler_rate_supported;
using rackboy::resampler::host_resampler_required_inputs;
using rackboy::resampler::host_resampler_reset_state;
using rackboy::resampler::kApuRateHz;
using rackboy::resampler::kHostResamplerNumTaps;
using rackboy::resampler::kNumChannels;

namespace {

// Small-number epsilon budget for DC / impulse / preservation
// assertions. The per-sample summation error is bounded by
// N_taps * float_eps = 257 * 1.2e-7 ~= 3e-5; we leave ~2x headroom.
constexpr float kDcEps = 1e-4f;
constexpr float kImpulseEps = 1e-5f;

// Configure APU Pulse1 at a given register-N tone, matching the
// SPIKE-5 stimulus shape. Pulled into a helper so rate-parameterised
// tests don't repeat the NR5x write sequence.
void configure_pulse1(rackboy_apu_t* apu, std::uint16_t freq_n) {
    rackboy_apu_write_reg(apu, 0xFF26, 0x80);  // NR52: APU on
    rackboy_apu_write_reg(apu, 0xFF25, 0x11);  // NR51: CH1 -> L+R
    rackboy_apu_write_reg(apu, 0xFF24, 0x77);  // NR50: max master vol
    rackboy_apu_write_reg(apu, 0xFF10, 0x00);  // NR10: no sweep
    rackboy_apu_write_reg(apu, 0xFF11, 0x80);  // NR11: 50% duty
    rackboy_apu_write_reg(apu, 0xFF12, 0xF0);  // NR12: vol 15, no env
    const std::uint8_t lo = static_cast<std::uint8_t>(freq_n & 0xFF);
    const std::uint8_t hi = static_cast<std::uint8_t>((freq_n >> 8) & 0x07);
    rackboy_apu_write_reg(apu, 0xFF13, lo);
    rackboy_apu_write_reg(apu, 0xFF14, static_cast<std::uint8_t>(0x80 | hi));
}

// Drive the APU / resampler pipeline until `n_out` host-rate samples
// per channel have been produced, consuming however many APU samples
// the current L/M phase schedule requires. Internally uses
// required_inputs / push / emit so the same code path the production
// audio callback exercises is under test.
std::array<std::vector<float>, kNumChannels>
run_stream(HostResampler* hr, rackboy_apu_t* apu, int n_out) {
    std::array<std::vector<float>, kNumChannels> out;
    for (int c = 0; c < kNumChannels; ++c) {
        out[c].reserve(static_cast<std::size_t>(n_out));
    }
    for (int m = 0; m < n_out; ++m) {
        const int need = host_resampler_required_inputs(hr);
        for (int i = 0; i < need; ++i) {
            float s[kNumChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
            rackboy_apu_run_samples(apu, 1,
                                    &s[0], &s[1], &s[2], &s[3]);
            host_resampler_push_input(hr, s[0], s[1], s[2], s[3]);
        }
        float y[kNumChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
        host_resampler_emit(hr, &y[0], &y[1], &y[2], &y[3]);
        for (int c = 0; c < kNumChannels; ++c) {
            out[c].push_back(y[c]);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("host_resampler_rate_supported matches the VCV rate set") {
    CHECK(host_resampler_rate_supported(44100u));
    CHECK(host_resampler_rate_supported(48000u));
    CHECK(host_resampler_rate_supported(96000u));
    CHECK(host_resampler_rate_supported(192000u));
    // Nearby but unsupported rates must NOT silently pass.
    CHECK_FALSE(host_resampler_rate_supported(22050u));
    CHECK_FALSE(host_resampler_rate_supported(32000u));
    CHECK_FALSE(host_resampler_rate_supported(88200u));
    CHECK_FALSE(host_resampler_rate_supported(176400u));
    CHECK_FALSE(host_resampler_rate_supported(384000u));
    CHECK_FALSE(host_resampler_rate_supported(0u));
}

TEST_CASE("host_resampler_init pins APU rate at 48 kHz and "
          "reduces L/M correctly") {
    struct RateExpectation {
        unsigned host_rate;
        int expected_L;
        int expected_M;
    };
    const RateExpectation rates[] = {
        {44100u,  147, 160},   // gcd(44100, 48000) = 300
        {48000u,    1,   1},   // identity
        {96000u,    2,   1},
        {192000u,   4,   1},
    };
    for (const auto& r : rates) {
        CAPTURE(r.host_rate);
        HostResampler hr{};
        REQUIRE(host_resampler_init(&hr, r.host_rate));
        CHECK(hr.apu_rate == kApuRateHz);
        CHECK(hr.host_rate == r.host_rate);
        CHECK(hr.L == r.expected_L);
        CHECK(hr.M == r.expected_M);
        CHECK(hr.half_taps == (kHostResamplerNumTaps - 1) / 2);
        // Kaiser window centre tap must land at 1.0 exactly for an
        // I0(beta)/I0(beta) evaluation at the symmetry point.
        CHECK(hr.win[hr.half_taps] == doctest::Approx(1.0f).epsilon(1e-6));
        // Window symmetry.
        for (int k = 0; k < hr.half_taps; ++k) {
            CHECK(hr.win[k] ==
                  doctest::Approx(hr.win[kHostResamplerNumTaps - 1 - k])
                      .epsilon(1e-6));
        }
    }
}

TEST_CASE("host_resampler_init rejects unsupported rates") {
    HostResampler hr{};
    // Stamp a sentinel so we can tell zero-init happened on failure.
    hr.host_rate = 0xDEADBEEFu;
    CHECK_FALSE(host_resampler_init(&hr, 22050u));
    // Failure path must leave the struct safe to touch via reset_state
    // (zeroed rather than partially initialised).
    CHECK(hr.host_rate == 0u);
    CHECK(hr.apu_rate == 0u);
    CHECK(hr.L == 0);
}

TEST_CASE("host_resampler_required_inputs reflects the L/M cadence") {
    // The first emit's required_inputs returns half_taps + 1 (the
    // kernel reads input indices [-half_taps, +half_taps]; only
    // indices in [0, input_count) are available). After that
    // cold-start burst the cadence settles into L/M:
    //   48 kHz  (L=M=1):  1, 1, 1, 1, ...
    //   96 kHz  (L=2):    1, 0, 1, 0, ...
    //   192 kHz (L=4):    1, 0, 0, 0, 1, 0, 0, 0, ...
    //   44.1 kHz (147/160): mostly 1, periodic 2.
    //
    // Cover both the startup burst and the equilibrium cadence.

    HostResampler hr{};

    auto burn_startup = [&](int expected_burst) {
        CHECK(host_resampler_required_inputs(&hr) == expected_burst);
        for (int i = 0; i < expected_burst; ++i) {
            host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        float y[4];
        host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
    };

    // 48 kHz startup: half_taps + 1 = 129 inputs for the first emit.
    REQUIRE(host_resampler_init(&hr, 48000u));
    burn_startup(hr.half_taps + 1);
    for (int m = 1; m < 100; ++m) {
        CHECK(host_resampler_required_inputs(&hr) == 1);
        host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
        float y[4];
        host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
    }

    // 96 kHz: first emit burst is at base = 0, latest = half_taps =>
    // need = half_taps + 1 = 129. Equilibrium: 100 outputs after
    // startup consume floor(100/2) = 50 inputs.
    REQUIRE(host_resampler_init(&hr, 96000u));
    burn_startup(hr.half_taps + 1);
    int pushes_total = 0;
    for (int m = 1; m <= 100; ++m) {
        const int need = host_resampler_required_inputs(&hr);
        CHECK(need >= 0);
        CHECK(need <= 1);
        for (int i = 0; i < need; ++i) {
            host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
            ++pushes_total;
        }
        float y[4];
        host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
    }
    // At 96 kHz (L=2) starting from phase 0: emit 1 (base=0), emit 2
    // (base=1, +1 push), emit 3 (base=1), emit 4 (base=2, +1 push).
    // Over 100 post-startup emits: 50 pushes.
    CHECK(pushes_total == 50);

    // 192 kHz (L=4): first emit burst at base=0, latest=half_taps.
    REQUIRE(host_resampler_init(&hr, 192000u));
    burn_startup(hr.half_taps + 1);
    pushes_total = 0;
    for (int m = 1; m <= 100; ++m) {
        const int need = host_resampler_required_inputs(&hr);
        CHECK(need >= 0);
        CHECK(need <= 1);
        for (int i = 0; i < need; ++i) {
            host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
            ++pushes_total;
        }
        float y[4];
        host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
    }
    // 100 emits @ L=4 after startup: 25 pushes.
    CHECK(pushes_total == 25);

    // 44.1 kHz (L=147, M=160): first emit's base = floor(0*160/147)
    // = 0; startup burst is half_taps + 1 = 129. Equilibrium over
    // 1470 emits (one full L-cycle): 1470 * 160 / 147 = 1600 pushes.
    REQUIRE(host_resampler_init(&hr, 44100u));
    burn_startup(hr.half_taps + 1);
    pushes_total = 0;
    for (int m = 1; m <= 1470; ++m) {
        const int need = host_resampler_required_inputs(&hr);
        CHECK(need >= 0);
        CHECK(need <= 2);
        for (int i = 0; i < need; ++i) {
            host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
            ++pushes_total;
        }
        float y[4];
        host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
    }
    // 1470 post-startup emits: 1470 * 160/147 = 1600 pushes. Startup
    // consumed 129 pushes for the first emit, whose base=0 --
    // subsequent emits advance base on the M/L cadence, so the net
    // pushes over the post-startup 1470 emits is exactly 1600
    // (one full L-cycle of the polyphase schedule).
    CHECK(pushes_total == 1600);
}

TEST_CASE("host_resampler preserves a DC input at every supported rate") {
    const unsigned rates[] = {44100u, 48000u, 96000u, 192000u};
    for (unsigned host_rate : rates) {
        CAPTURE(host_rate);
        HostResampler hr{};
        REQUIRE(host_resampler_init(&hr, host_rate));

        // Burn the kernel warmup: push half_taps samples of DC 1.0
        // so the filter's ring is pre-populated on the input side.
        constexpr float kDc = 0.5f;
        const int warmup = hr.half_taps;
        for (int i = 0; i < warmup; ++i) {
            host_resampler_push_input(&hr, kDc, -kDc, kDc, -kDc);
        }

        // Now drive the resampler for a healthy run, checking that
        // the middle-of-run outputs match DC exactly. Skip the first
        // few emits (the right half of the kernel is still warming
        // up because output_count starts at 0, not -half_taps).
        const int n_out = 2 * hr.L + 512;
        std::vector<float> y_ch[kNumChannels];
        for (int c = 0; c < kNumChannels; ++c) { y_ch[c].reserve(n_out); }

        for (int m = 0; m < n_out; ++m) {
            const int need = host_resampler_required_inputs(&hr);
            for (int i = 0; i < need; ++i) {
                host_resampler_push_input(&hr, kDc, -kDc, kDc, -kDc);
            }
            float y[4];
            host_resampler_emit(&hr, &y[0], &y[1], &y[2], &y[3]);
            for (int c = 0; c < kNumChannels; ++c) { y_ch[c].push_back(y[c]); }
        }

        // The filter's group delay is half_taps at the APU rate, so
        // equilibrium outputs land roughly at output index
        // half_taps * L / M and onward. Check outputs well past that
        // frontier; they should all read the DC input exactly.
        const int first_equil =
            static_cast<int>(static_cast<long long>(hr.half_taps) *
                             hr.L / hr.M + 32);
        for (int m = first_equil; m < n_out - 8; ++m) {
            CHECK(y_ch[0][m] == doctest::Approx( kDc).epsilon(kDcEps));
            CHECK(y_ch[1][m] == doctest::Approx(-kDc).epsilon(kDcEps));
            CHECK(y_ch[2][m] == doctest::Approx( kDc).epsilon(kDcEps));
            CHECK(y_ch[3][m] == doctest::Approx(-kDc).epsilon(kDcEps));
        }
    }
}

TEST_CASE("host_resampler_emit recovers the input verbatim at 48 kHz") {
    // L=1, M=1 path. At integer phase the kernel collapses to a
    // single-tap identity (win[half_taps] * fc * sinc(0) = 1), so
    // the resampler is bit-close to a delay line.
    HostResampler hr{};
    REQUIRE(host_resampler_init(&hr, 48000u));

    std::vector<float> xs;
    xs.reserve(600);
    for (int i = 0; i < 600; ++i) {
        // Distinct non-trivial test pattern.
        xs.push_back(0.1f * std::sin(0.02f * static_cast<float>(i)));
    }

    // Warm up: push half_taps zeros so the first real emit lands at
    // a fully-populated kernel.
    for (int i = 0; i < hr.half_taps; ++i) {
        host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Drive inputs and collect outputs. At L=1/M=1 every emit
    // consumes exactly one push.
    std::vector<float> y;
    y.reserve(xs.size());
    for (float v : xs) {
        const int need = host_resampler_required_inputs(&hr);
        CHECK(need == 1);
        host_resampler_push_input(&hr, v, 0.0f, 0.0f, 0.0f);
        float out[4];
        host_resampler_emit(&hr, &out[0], &out[1], &out[2], &out[3]);
        y.push_back(out[0]);
    }

    // Group delay at 48 kHz = half_taps outputs before the first
    // non-zero value comes out. Starting at y[half_taps] the output
    // must match the delay-line reading x[m - half_taps].
    // Pre-warm zeros were already pushed, so y[m] corresponds to
    // x[m - half_taps] once m >= half_taps.
    const int delay = hr.half_taps;
    for (int m = delay; m < static_cast<int>(xs.size()); ++m) {
        CHECK(y[m] ==
              doctest::Approx(xs[static_cast<std::size_t>(m - delay)])
                  .epsilon(kImpulseEps));
    }
}

TEST_CASE("host_resampler emits a Kaiser-windowed-sinc kernel on an impulse") {
    // At 96 kHz (L=2, M=1) a unit impulse pushed into the centre of
    // a zero-filled history produces outputs that trace a sampled
    // copy of the Kaiser-windowed-sinc kernel around the impulse's
    // position. Check a handful of on-grid and off-grid output
    // samples match the closed-form kernel.
    HostResampler hr{};
    REQUIRE(host_resampler_init(&hr, 96000u));

    // Pre-roll so the first real emit already has a symmetric ring.
    for (int i = 0; i < hr.half_taps; ++i) {
        host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Push one impulse amid zeros. We keep pushing zeros for the
    // right half so the kernel window is symmetric around the
    // impulse. After this block, input_count = 2 * half_taps + 1.
    host_resampler_push_input(&hr, 1.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < hr.half_taps; ++i) {
        host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Emit output_count = 0 .. 2*(2*half_taps+1) = a full 2x upsample
    // of the span, so the impulse response shows up centred in the
    // output array.
    const int n_out = 2 * (2 * hr.half_taps + 1);
    std::vector<float> y;
    y.reserve(n_out);
    for (int m = 0; m < n_out; ++m) {
        // 96 kHz cadence: 1, 0, 1, 0, ... After the pre-roll +
        // impulse above, required_inputs returns 0 until more
        // pushes would be needed. We have enough history for the
        // full n_out; any remaining need must read 0.
        int need = host_resampler_required_inputs(&hr);
        while (need > 0) {
            host_resampler_push_input(&hr, 0.0f, 0.0f, 0.0f, 0.0f);
            need = host_resampler_required_inputs(&hr);
        }
        float out[4];
        host_resampler_emit(&hr, &out[0], &out[1], &out[2], &out[3]);
        y.push_back(out[0]);
    }

    // The impulse lives at input index half_taps. At L=2 the output
    // samples around its centre are:
    //   y[2*half_taps] == fc * sinc(0) * win[half_taps] == 1.0
    //   y[2*half_taps + 1] == fc * sinc(fc * 0.5) * win[half_taps]
    // where fc = 1.0 at 96 kHz upsampling. The left neighbour is
    // the mirrored value on the other side.
    const int centre = 2 * hr.half_taps;
    CHECK(y[centre] == doctest::Approx(1.0f).epsilon(1e-4));

    // For an impulse at input index `half_taps`, the only non-zero
    // tap read by emit(m) is k_active = 2*half_taps - base, where
    // base = floor(m * M / L). The produced output is
    //   y[m] = fc * sinc(fc * tau) * win[k_active]
    // with tau = (base + frac) - half_taps = (base - half_taps) + frac.
    //
    // This is the "one-hot impulse response" evaluation of a
    // Kaiser-windowed-sinc kernel, parameterised by output index.
    auto sinc_pi_d = [](double x) -> double {
        if (x == 0.0) { return 1.0; }
        const double pix = 3.14159265358979323846 * x;
        return std::sin(pix) / pix;
    };
    auto expected_at = [&hr, &sinc_pi_d](int m) -> double {
        const long long num = static_cast<long long>(m) *
                              static_cast<long long>(hr.M);
        long long base = num / static_cast<long long>(hr.L);
        long long rem = num - base * static_cast<long long>(hr.L);
        if (rem < 0) { base -= 1; rem += hr.L; }
        const double frac = static_cast<double>(rem) /
                            static_cast<double>(hr.L);
        const long long k_active_ll =
            2LL * static_cast<long long>(hr.half_taps) - base;
        if (k_active_ll < 0 || k_active_ll >= kHostResamplerNumTaps) {
            return 0.0;
        }
        const int k_active = static_cast<int>(k_active_ll);
        const double tau =
            static_cast<double>(base - hr.half_taps) + frac;
        return hr.fc * sinc_pi_d(hr.fc * tau) *
               static_cast<double>(hr.win[k_active]);
    };

    const int offsets[] = {-4, -3, -2, -1, 1, 2, 3, 4};
    for (int j : offsets) {
        const double expected = expected_at(centre + j);
        CAPTURE(j);
        CHECK(static_cast<double>(y[centre + j]) ==
              doctest::Approx(expected).epsilon(1e-3));
    }
}

TEST_CASE("host_resampler meets SPIKE-3 acceptance end-to-end "
          "(D-012 binding criteria)") {
    // Production acceptance test for bead rackboy-s20, using the
    // BH4 / Goertzel / exact-harmonic-exclusion helpers promoted
    // from src/spike/apu_bandlimited/main.cpp per D-012 item 4
    // (bead rackboy-dmq).
    //
    // Drives the pinned-48 kHz APU with a near-1 kHz Pulse1
    // stimulus and pulls it through the production HostResampler
    // at each of the four supported host rates. Measures:
    //
    //   * Fundamental amplitude with Goertzel at the exact
    //     stimulus frequency (no scalloping).
    //   * Non-harmonic spur floor with a BH4-windowed FFT plus
    //     exact-frequency odd-harmonic exclusion (Pulse1 at 50%
    //     duty has only odd harmonics).
    //
    // Asserts SPIKE-3 criterion 1 (inter-rate peak spread <= 0.25
    // dB) and criterion 2 (worst non-harmonic spur <= -60 dBFS)
    // EXACTLY -- no smoke-test widening. A regression here is the
    // signal that the bandlimited pipeline has broken; the test
    // MUST fail loudly rather than have its tolerances widened.
    // Update docs/DECISIONS.md if a legitimate architectural
    // change shifts the numbers.
    struct RateCase { unsigned host_rate; };
    const RateCase cases[] = { {44100u}, {48000u}, {96000u}, {192000u} };

    // Pulse1 frequency register N = 1917 -> f = 131072/(2048-1917)
    // = 131072/131 = 1000.549... Hz. Same stimulus the SPIKE-5
    // harness uses, so thresholds below compare apples-to-apples
    // with the spike's report.
    constexpr std::uint16_t kPulse1FreqN = 1917;
    const double kFundamentalHz = 131072.0 / (2048.0 - 1917.0);

    // 32768-point FFT matches the spike's resolution. At 44.1 kHz
    // that is 1.35 Hz / bin; at 192 kHz it is 5.86 Hz / bin --
    // comfortably finer than the BH4 main-lobe width.
    constexpr std::size_t kFftLen = 32768;
    // Transient skip: the resampler's Kaiser-windowed-sinc kernel
    // has group delay half_taps = 128 APU samples = 2.67 ms at
    // 48 kHz. Combined with APU envelope warmup, 1024 host
    // samples of lead-in clears the transient at every rate.
    constexpr int kTransientOut = 1024;
    // BH4 main-lobe width spans ~4 bins; a 5-bin half-width
    // exclusion window is the spike's SPIKE-5 convention and
    // comfortably avoids counting main-lobe skirt as a spur.
    constexpr std::size_t kExclusionHalfWidthBins = 5;

    // Criteria from docs/DECISIONS.md D-012 (binding).
    constexpr double kCriterion1MaxSpreadDb = 0.25;
    constexpr double kCriterion2MaxSpurDb   = -60.0;

    std::vector<double> peak_db_per_rate;
    peak_db_per_rate.reserve(4);
    std::vector<double> spur_db_per_rate;
    spur_db_per_rate.reserve(4);

    for (const auto& tc : cases) {
        CAPTURE(tc.host_rate);
        rackboy_apu_t apu{};
        rackboy_apu_init(&apu, kApuRateHz);
        configure_pulse1(&apu, kPulse1FreqN);

        HostResampler hr{};
        REQUIRE(host_resampler_init(&hr, tc.host_rate));

        const int n_out =
            kTransientOut + static_cast<int>(kFftLen);
        auto streamed = run_stream(&hr, &apu, n_out);

        // Post-transient slice on ch1 (Pulse1 host-rate output).
        std::vector<float> analysis(kFftLen, 0.0f);
        for (std::size_t i = 0; i < kFftLen; ++i) {
            analysis[i] = streamed[0][
                static_cast<std::size_t>(kTransientOut) + i];
        }

        // --- Fundamental via Goertzel at exact f_hz -----------
        const double peak_db =
            rackboy::resampler::measurement::goertzel_amplitude_db(
                analysis, kFundamentalHz, tc.host_rate);
        peak_db_per_rate.push_back(peak_db);

        // --- Spur floor via BH4 + exact harmonic exclusion ----
        std::vector<float> bh4_analysis(analysis);
        rackboy::resampler::measurement::apply_bh4_window(bh4_analysis);

        std::vector<std::complex<float>> cmplx(kFftLen);
        for (std::size_t i = 0; i < kFftLen; ++i) {
            cmplx[i] = std::complex<float>(bh4_analysis[i], 0.0f);
        }
        rackboy::spike::vcv_resampler::fft_forward(cmplx);
        const std::vector<float> spectrum_db =
            rackboy::resampler::measurement::bh4_magnitude_db_fs(cmplx);

        const int max_h =
            rackboy::resampler::measurement::max_odd_harmonic_in_band(
                kFundamentalHz, tc.host_rate);

        const double spur_db =
            rackboy::resampler::measurement::spur_floor_exact(
                spectrum_db, kFundamentalHz, tc.host_rate, kFftLen,
                kExclusionHalfWidthBins, max_h,
                /*odd_harmonics_only=*/true);
        spur_db_per_rate.push_back(spur_db);

        CAPTURE(peak_db);
        CAPTURE(spur_db);
        // Criterion 2 is per-rate.
        CHECK(spur_db < kCriterion2MaxSpurDb);
    }

    // Criterion 1: inter-rate peak spread across the four rates.
    double lo = 1e9, hi = -1e9;
    for (double v : peak_db_per_rate) {
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
    }
    CAPTURE(lo);
    CAPTURE(hi);
    const double spread_db = hi - lo;
    CAPTURE(spread_db);
    CHECK(spread_db <= kCriterion1MaxSpreadDb);
}

TEST_CASE("host_resampler re-init on rate change updates L/M and "
          "zeroes the phase counters") {
    // Rackboy::onSampleRateChange calls reset_runtime_state which
    // re-inits the resampler at a new host rate. Exercising that
    // mid-stream here catches ring-buffer / coefficient lifetime
    // bugs that `onSampleRateChange` would otherwise hit live.
    HostResampler hr{};
    REQUIRE(host_resampler_init(&hr, 48000u));
    CHECK(hr.L == 1);
    CHECK(hr.M == 1);

    // Push some data to simulate a running stream, then re-init.
    for (int i = 0; i < 64; ++i) {
        host_resampler_push_input(&hr, 1.0f, -1.0f, 0.25f, -0.25f);
    }
    CHECK(hr.input_count == 64);

    REQUIRE(host_resampler_init(&hr, 96000u));
    CHECK(hr.L == 2);
    CHECK(hr.M == 1);
    CHECK(hr.host_rate == 96000u);
    CHECK(hr.apu_rate == kApuRateHz);
    CHECK(hr.input_count == 0);
    CHECK(hr.output_count == 0);

    // Switching to an unsupported rate wipes the struct.
    CHECK_FALSE(host_resampler_init(&hr, 22050u));
    CHECK(hr.L == 0);
    CHECK(hr.host_rate == 0u);
}

TEST_CASE("host_resampler_reset_state on a freshly initialised "
          "resampler is a no-op") {
    // The reset path must be safe on an unused instance (e.g. when
    // Rack calls onReset before the audio thread has ever run).
    HostResampler hr{};
    REQUIRE(host_resampler_init(&hr, 48000u));
    CHECK(hr.input_count == 0);
    CHECK(hr.output_count == 0);

    host_resampler_reset_state(&hr);  // no-op on a fresh instance
    CHECK(hr.input_count == 0);
    CHECK(hr.output_count == 0);
    CHECK(hr.L == 1);   // filter state untouched
    CHECK(hr.M == 1);
}

TEST_CASE("host_resampler_reset_state clears history without touching filter") {
    HostResampler hr{};
    REQUIRE(host_resampler_init(&hr, 96000u));

    // Push a few non-zero inputs, then reset.
    for (int i = 0; i < 64; ++i) {
        host_resampler_push_input(&hr, 1.0f, -1.0f, 0.25f, -0.25f);
    }
    CHECK(hr.input_count == 64);

    host_resampler_reset_state(&hr);
    CHECK(hr.input_count == 0);
    CHECK(hr.output_count == 0);

    // Filter coefficients stay intact (centre tap still 1.0).
    CHECK(hr.win[hr.half_taps] == doctest::Approx(1.0f).epsilon(1e-6));

    // History reads after reset give 0 regardless of ring slot.
    for (int c = 0; c < kNumChannels; ++c) {
        for (int i = 0;
             i < rackboy::resampler::kHostResamplerHistorySize;
             ++i) {
            CHECK(hr.history[c][i] == 0.0f);
        }
    }
}
