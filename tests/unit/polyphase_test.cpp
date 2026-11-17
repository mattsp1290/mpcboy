// WP3 tests 4-5: PolyphaseResampler48To441 vs the vendored HostResampler, and
// the 1 kHz Pulse 1 spur floor.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "doctest/doctest.h"

extern "C" {
#include "rackboy_apu.h"
}

#include "polyphase_resampler.h"
#include "rackboy_resampler.hpp"
#include "resampler/measurement.hpp"

namespace {

using mpcboy::PolyphaseResampler48To441;
namespace rr = rackboy::resampler;

std::vector<float> noise_signal(int n)
{
    std::vector<float> v(static_cast<size_t>(n));
    uint32_t s = 0xC0FFEEu;
    for (auto& x : v)
    {
        s = s * 1664525u + 1013904223u;
        x = (static_cast<float>(s >> 8) / 16777216.0f - 0.5f);   // +-0.5
    }
    return v;
}

std::vector<float> square_1k(int n)
{
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = (i % 48) < 24 ? 0.5f : -0.5f;   // 1 kHz at 48 kHz
    return v;
}

// Reference stream through the vendored HostResampler (channel 1 only).
std::vector<float> run_reference(const std::vector<float>& in, int nOut)
{
    auto hr = std::make_unique<rr::HostResampler>();
    REQUIRE(rr::host_resampler_init(hr.get(), 44100));
    std::vector<float> out(static_cast<size_t>(nOut));
    size_t pushed = 0;
    for (int i = 0; i < nOut; ++i)
    {
        int need = rr::host_resampler_required_inputs(hr.get());
        while (need-- > 0)
        {
            const float x = pushed < in.size() ? in[pushed] : 0.0f;
            rr::host_resampler_push_input(hr.get(), x, 0, 0, 0);
            ++pushed;
        }
        float o = 0;
        rr::host_resampler_emit(hr.get(), &o, nullptr, nullptr, nullptr);
        out[static_cast<size_t>(i)] = o;
    }
    return out;
}

// Same stream through the polyphase resampler in host blocks of `blockSizes`.
std::vector<float> run_polyphase(const std::vector<float>& in, int nOut, const int* blockSizes, int nSizes)
{
    auto p = std::make_unique<PolyphaseResampler48To441>();
    p->reset(0);
    std::vector<float> out(static_cast<size_t>(nOut));
    size_t pos = 0;
    int produced = 0, bi = 0;
    while (produced < nOut)
    {
        int n = blockSizes[bi++ % nSizes];
        if (n > nOut - produced) n = nOut - produced;
        const int need = p->inputsNeeded(n);
        std::vector<float> chunk(static_cast<size_t>(need), 0.0f);
        for (int i = 0; i < need; ++i) chunk[static_cast<size_t>(i)] = pos + static_cast<size_t>(i) < in.size() ? in[pos + static_cast<size_t>(i)] : 0.0f;
        pos += static_cast<size_t>(need);
        std::vector<float> dummy(chunk);
        std::vector<float> outR(static_cast<size_t>(n));
        p->process(chunk.data(), dummy.data(), need, out.data() + produced, outR.data(), n);
        produced += n;
    }
    return out;
}

double diff_rms_dbfs(const std::vector<float>& a, const std::vector<float>& b, int from)
{
    double acc = 0.0;
    int n = 0;
    for (size_t i = static_cast<size_t>(from); i < a.size(); ++i, ++n)
    {
        const double d = static_cast<double>(a[i]) - b[i];
        acc += d * d;
    }
    const double rms = std::sqrt(acc / n);
    return rms == 0.0 ? -300.0 : 20.0 * std::log10(rms);
}

}  // namespace

TEST_CASE("polyphase 4: output equals the vendored HostResampler (-100 dBFS RMS) for noise and a 1 kHz square")
{
    constexpr int kOut = 8192;
    const int in_n = kOut * 160 / 147 + 400;
    const int sizes[] = {32, 64, 96, 128, 192, 256, 512, 128, 100};
    const std::vector<float> sigs[2] = {noise_signal(in_n), square_1k(in_n)};
    const char* names[2] = {"white noise", "1 kHz square"};
    for (int k = 0; k < 2; ++k)
    {
        const auto ref = run_reference(sigs[k], kOut);
        const auto got = run_polyphase(sigs[k], kOut, sizes, 9);
        const double db = diff_rms_dbfs(ref, got, 512);
        std::printf("[polyphase vs HostResampler] %s: diff RMS %.2f dBFS\n", names[k], db);
        CHECK_MESSAGE(db < -100.0, names[k]);
    }
}

TEST_CASE("polyphase: block size does not change the output (bit-exact) and counts are consistent")
{
    constexpr int kOut = 6000;
    const int in_n = kOut * 160 / 147 + 400;
    const auto sig = noise_signal(in_n);
    const int a[] = {128};
    const int b[] = {32, 512, 96, 64, 192, 256, 100};
    const auto x = run_polyphase(sig, kOut, a, 1);
    const auto y = run_polyphase(sig, kOut, b, 7);
    CHECK(std::memcmp(x.data(), y.data(), sizeof(float) * kOut) == 0);

    PolyphaseResampler48To441 p;
    p.reset(128);   // the engine's primed start
    int total = 0;
    for (int blk = 0; blk < 200; ++blk)
    {
        const int need = p.inputsNeeded(128);
        CHECK(need >= 136);
        CHECK(need <= 141);
        std::vector<float> z(static_cast<size_t>(need), 0.0f), oL(128), oR(128);
        p.process(z.data(), z.data(), need, oL.data(), oR.data(), 128);
        total += need;
    }
    CHECK(p.inputCount() == 128 + total);
    CHECK(p.outputCount() == 200 * 128);
}

TEST_CASE("polyphase 5: 1 kHz Pulse 1 square has no spur above -70 dBFS outside the harmonics")
{
    static rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    constexpr uint16_t kN = 1917;   // 131072 / (2048 - 1917) = 1000.55 Hz
    const double f0 = 131072.0 / (2048.0 - kN);
    rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF11, 0x80);
    rackboy_apu_write_reg(&apu, 0xFF12, 0xF0);
    rackboy_apu_write_reg(&apu, 0xFF13, kN & 0xFF);
    rackboy_apu_write_reg(&apu, 0xFF14, static_cast<uint8_t>(0x80 | (kN >> 8)));

    constexpr int kFft = 32768, kSkip = 1024, kOut = kFft + kSkip;
    PolyphaseResampler48To441 rs;
    rs.reset(0);
    std::vector<float> out(kOut), outR(kOut);
    std::vector<float> aL(1024), aR(1024);
    int produced = 0;
    while (produced < kOut)
    {
        const int n = std::min(128, kOut - produced);
        const int need = rs.inputsNeeded(n);
        rackboy_apu_run_stereo(&apu, static_cast<uint32_t>(need), aL.data(), aR.data());
        rs.process(aL.data(), aR.data(), need, out.data() + produced, outR.data() + produced, n);
        produced += n;
    }
    std::vector<float> win(out.begin() + kSkip, out.begin() + kSkip + kFft);
    rr::measurement::apply_bh4_window(win);
    std::vector<std::complex<float>> cm(kFft);
    for (int i = 0; i < kFft; ++i) cm[static_cast<size_t>(i)] = std::complex<float>(win[static_cast<size_t>(i)], 0.0f);
    rackboy::spike::vcv_resampler::fft_forward(cm);
    const auto db = rr::measurement::bh4_magnitude_db_fs(cm);
    const int maxH = rr::measurement::max_odd_harmonic_in_band(f0, 44100);
    const double spur = rr::measurement::spur_floor_exact(db, f0, 44100, kFft, 5, maxH, true);
    std::printf("[polyphase spur] worst non-harmonic spur %.2f dBFS\n", spur);
    CHECK(spur < -70.0);
}
