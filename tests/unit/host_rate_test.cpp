// Host sample rate support: tick timing, audio sanity, large blocks, rate changes
// through the plugin, unsupported rates. 44.1 kHz bit-identity is covered by the
// existing suites (golden regression, engine tests) which all run at 44.1 kHz.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "doctest/doctest.h"
#include "dc_blocker.h"
#include "engine_test_support.h"
#include "plugin_test_support.h"
#include "rackboy_resampler.hpp"
#include "rate_resampler.h"

using namespace mpcboy;
using namespace mpcboy_test;

namespace {

constexpr uint16_t NR13 = 0xFF13, NR14 = 0xFF14;

// Engine at `sr` with its own TickClock on (Sync/Note, 24 per beat), spy installed.
struct RateClockRig
{
    std::unique_ptr<Engine> e;
    std::vector<W> log;
    long long pos = 0;
    double bpm = 120.0;

    explicit RateClockRig(int sr)
    {
        e = std::make_unique<Engine>();
        REQUIRE(e->setHostRate(sr));
        e->reset();
        rackboy_apu_set_write_spy(e->apu(), &RateClockRig::spy, this);
    }
    ~RateClockRig() { rackboy_apu_set_write_spy(e->apu(), nullptr, nullptr); }
    static void spy(void* u, uint16_t a, uint8_t v)
    {
        auto* r = static_cast<RateClockRig*>(u);
        r->log.push_back({r->e->apuFrame(), a, v});
    }

    void run(long long frames, std::vector<Ev> evs, int block)
    {
        std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
        std::vector<EngineMidiEvent> midi;
        size_t ei = 0;
        const long long end = pos + frames;
        while (pos < end)
        {
            const int n = static_cast<int>(std::min<long long>(block, end - pos));
            midi.clear();
            while (ei < evs.size() && evs[ei].frame < pos + n)
            {
                EngineMidiEvent m;
                m.offset = static_cast<int>(evs[ei].frame - pos);
                m.status = evs[ei].status; m.data1 = evs[ei].d1; m.data2 = evs[ei].d2;
                midi.push_back(m);
                ++ei;
            }
            EngineBlock b;
            b.midi = midi.empty() ? nullptr : midi.data();
            b.numMidi = static_cast<int>(midi.size());
            b.bpm = bpm; b.tempoValid = true;
            e->render(b, L.data(), R.data(), n);
            pos += n;
        }
    }
};

double rateRms(const std::vector<float>& v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to && i < v.size(); ++i) acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt(acc / static_cast<double>(to - from));
}

bool allFinite(const std::vector<float>& v)
{
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace

TEST_CASE("host rate: supported set")
{
    CHECK(Engine::hostRateSupported(44100));
    CHECK(Engine::hostRateSupported(48000));
    CHECK(Engine::hostRateSupported(96000));
    CHECK(Engine::hostRateSupported(192000));
    CHECK_FALSE(Engine::hostRateSupported(88200));
    CHECK_FALSE(Engine::hostRateSupported(22050));
    CHECK_FALSE(Engine::hostRateSupported(0));
    Engine e;
    CHECK_FALSE(e.setHostRate(88200));
    CHECK(e.hostRate() == 44100);   // unchanged
}

TEST_CASE("host rate: table ticks land within 2 host frames of k * sr * 60 / (bpm * 24) at 44.1/48/96/192 kHz")
{
    const int rates[4] = {44100, 48000, 96000, 192000};
    const int blocks[4] = {128, 512, 1000, 2048};   // up to a typical large Live buffer
    for (int sr : rates)
        for (int blk : blocks)
        {
            CAPTURE(sr);
            CAPTURE(blk);
            RateClockRig r(sr);
            EngineSettings s;
            s.ch[0].tableSlot = 0;
            for (int i = 1; i < 4; ++i) s.ch[i].enabled = false;
            r.e->applySettings(s);
            setTable(r.e->ownBank().tables[0], octave());
            const long long noteFrame = 1234;
            r.run(static_cast<long long>(sr) * 4, {noteOn(noteFrame, 60)}, blk);
            const auto fw = freqWrites(r.log, NR13, NR14);
            REQUIRE(fw.size() > 30);
            CHECK(fw[0].trigger);
            const double period = sr * 60.0 / (r.bpm * 24.0);
            double worst = 0.0;
            for (size_t k = 0; k + 1 < fw.size(); ++k)
            {
                const double host = static_cast<double>(fw[1 + k].apu) * sr / 48000.0;
                const double expected = std::round(static_cast<double>(k) * period) + static_cast<double>(noteFrame);
                worst = std::max(worst, std::fabs(host - expected));
            }
            std::printf("[host rate] sr %d block %d: worst tick deviation %.2f host frames over %zu ticks\n", sr, blk,
                        worst, fw.size() - 1);
            CHECK(worst <= 2.0);
            CHECK(r.e->tickClock().droppedTicks() == 0);
            CHECK(r.e->tickClock().sampleRate() == doctest::Approx(sr));
        }
}

TEST_CASE("host rate: audio is finite, non-silent and at the right pitch at 44.1/48/96/192 kHz, in blocks up to 4096")
{
    const int rates[4] = {44100, 48000, 96000, 192000};
    for (int sr : rates)
        for (int blk : {128, 4096})
        {
            CAPTURE(sr);
            CAPTURE(blk);
            Rig r;
            REQUIRE(r.e->setHostRate(sr));
            r.e->reset();
            r.only(0);
            r.apply();
            r.run(static_cast<long long>(sr) * 2, {noteOn(0, 60)}, blk);
            REQUIRE(r.L.size() == static_cast<size_t>(sr) * 2);
            CHECK(allFinite(r.L));
            CHECK(allFinite(r.R));
            CHECK(rateRms(r.L, static_cast<size_t>(sr) / 2, static_cast<size_t>(sr) * 2) > 0.01);
            // Rising zero crossings -> frequency (note 60 on a square channel).
            double first = -1, last = -1;
            int n = 0;
            for (size_t i = static_cast<size_t>(sr) / 2 + 1; i < r.L.size(); ++i)
                if (r.L[i - 1] < 0.0f && r.L[i] >= 0.0f)
                {
                    const double t = static_cast<double>(i - 1) + (0.0 - r.L[i - 1]) / (static_cast<double>(r.L[i]) - r.L[i - 1]);
                    if (first < 0) first = t;
                    last = t;
                    ++n;
                }
            REQUIRE(n > 10);
            const double hz = (n - 1) * sr / (last - first);
            std::printf("[host rate] sr %d block %d: %.2f Hz\n", sr, blk, hz);
            CHECK(hz == doctest::Approx(261.63).epsilon(0.02));
        }
}

TEST_CASE("host rate: the plugin applies prepare() rate changes between blocks and stays valid")
{
    auto p = std::make_unique<MpcBoyPlugin>();
    std::vector<float> L, R;
    auto block = [&](double sr, int n, std::vector<mpc::MidiEvent> midi) {
        const size_t old = L.size();
        L.resize(old + static_cast<size_t>(n));
        R.resize(old + static_cast<size_t>(n));
        float* outs[2] = {L.data() + old, R.data() + old};
        mpc::ProcessContext ctx;
        ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.midi = midi.empty() ? nullptr : midi.data();
        ctx.numMidiEvents = static_cast<int>(midi.size());
        ctx.sampleRate = sr;
        ctx.transport.valid = true; ctx.transport.bpm = 120.0; ctx.transport.tempoValid = true;
        p->process(ctx);
    };
    struct Seg { double sr; bool expectSound; };
    const Seg segs[] = {{44100.0, true}, {48000.0, true}, {96000.0, true}, {88200.0, false}, {48000.0, true}, {44100.0, true}};
    for (const Seg& sg : segs)
    {
        CAPTURE(sg.sr);
        p->prepare(sg.sr, 512);
        const size_t start = L.size();
        block(sg.sr, 256, {mpc::MidiEvent::noteOn(0, 60, 100, 10)});   // applied rate + reset happen here
        for (int i = 0; i < 40; ++i) block(sg.sr, 512, {});
        REQUIRE(allFinite(L));
        const double rms = rateRms(L, start + 4096, L.size());
        if (sg.expectSound) CHECK(rms > 0.001);
        else
        {
            CHECK(rms == 0.0);   // unsupported rate: exact silence
            for (size_t i = start; i < L.size(); ++i) { REQUIRE(L[i] == 0.0f); REQUIRE(R[i] == 0.0f); }
        }
        // release before the next segment's reset
        block(sg.sr, 64, {mpc::MidiEvent::noteOff(0, 60, 0)});
    }
}

TEST_CASE("host rate: plugin at 48 kHz is the same engine path as 44.1 kHz output level (no resampler gain error)")
{
    auto render = [](int sr) {
        Engine e;
        REQUIRE(e.setHostRate(sr));
        e.reset();
        EngineSettings s;
        for (int i = 1; i < 4; ++i) s.ch[i].enabled = false;
        e.applySettings(s);
        std::vector<float> L(static_cast<size_t>(sr)), R(static_cast<size_t>(sr));
        EngineMidiEvent on;
        on.offset = 0; on.status = 0x90; on.data1 = 60; on.data2 = 100;
        for (int c0 = 0; c0 < sr; c0 += 512)
        {
            const int n = std::min(512, sr - c0);
            EngineBlock b;
            b.midi = c0 == 0 ? &on : nullptr;
            b.numMidi = c0 == 0 ? 1 : 0;
            e.render(b, L.data() + c0, R.data() + c0, n);
        }
        return rateRms(L, static_cast<size_t>(sr) / 2, static_cast<size_t>(sr));
    };
    const double a = render(44100), b = render(48000), c = render(96000);
    std::printf("[host rate] rms 44.1k %.4f, 48k %.4f, 96k %.4f\n", a, b, c);
    CHECK(b == doctest::Approx(a).epsilon(0.05));
    CHECK(c == doctest::Approx(a).epsilon(0.05));
}

TEST_CASE("host rate: the plugin path at 192 kHz (prepare, then process) is finite and non-silent")
{
    auto p = std::make_unique<MpcBoyPlugin>();
    std::vector<float> L, R;
    auto block = [&](double sr, int n, std::vector<mpc::MidiEvent> midi) {
        const size_t old = L.size();
        L.resize(old + static_cast<size_t>(n));
        R.resize(old + static_cast<size_t>(n));
        float* outs[2] = {L.data() + old, R.data() + old};
        mpc::ProcessContext ctx;
        ctx.outputs = outs; ctx.numOutputs = 2; ctx.numFrames = n;
        ctx.midi = midi.empty() ? nullptr : midi.data();
        ctx.numMidiEvents = static_cast<int>(midi.size());
        ctx.sampleRate = sr;
        ctx.transport.valid = true; ctx.transport.bpm = 120.0; ctx.transport.tempoValid = true;
        p->process(ctx);
    };
    p->prepare(192000.0, 512);
    block(192000.0, 256, {mpc::MidiEvent::noteOn(0, 60, 100, 10)});   // rate change + reset applied here
    for (int i = 0; i < 400; ++i) block(192000.0, 512, {});            // ~1.07 s
    REQUIRE(allFinite(L));
    REQUIRE(allFinite(R));
    const double rmsL = rateRms(L, 48000, L.size()), rmsR = rateRms(R, 48000, R.size());
    std::printf("[host rate] plugin at 192 kHz: rms L %.4f R %.4f\n", rmsL, rmsR);
    CHECK(rmsL > 0.001);
    CHECK(rmsR > 0.001);
}

TEST_CASE("host rate: RateResampler matches the vendored HostResampler within -100 dBFS at 96 and 192 kHz")
{
    for (int sr : {96000, 192000})
    {
        CAPTURE(sr);
        constexpr int kIn = 24000;   // 0.5 s of APU-rate input
        // Broadband content: several sines plus a deterministic LCG noise, peak around 0.9.
        std::vector<float> inL(kIn), inR(kIn);
        uint32_t lcg = 12345u;
        for (int i = 0; i < kIn; ++i)
        {
            lcg = lcg * 1664525u + 1013904223u;
            const double noise = (static_cast<double>(lcg >> 8) / 8388608.0 - 1.0) * 0.1;
            const double t = i / 48000.0;
            const double v = 0.3 * std::sin(2 * 3.14159265358979 * 440.0 * t) + 0.2 * std::sin(2 * 3.14159265358979 * 5000.0 * t)
                             + 0.2 * std::sin(2 * 3.14159265358979 * 17000.0 * t) + noise;
            inL[i] = static_cast<float>(v);
            inR[i] = static_cast<float>(-0.7 * v);
        }

        // Reference: vendored HostResampler, primed with 128 zero frames as the engine does.
        rackboy::resampler::HostResampler hr{};
        REQUIRE(rackboy::resampler::host_resampler_init(&hr, static_cast<unsigned>(sr)));
        for (int i = 0; i < RateResampler::kHalfTaps; ++i) rackboy::resampler::host_resampler_push_input(&hr, 0, 0, 0, 0);
        const int nOut = static_cast<int>(static_cast<long long>(kIn - 600) * sr / 48000);   // stay inside the covered input
        std::vector<float> refL(static_cast<size_t>(nOut)), refR(static_cast<size_t>(nOut));
        int pushed = 0;
        for (int i = 0; i < nOut; ++i)
        {
            while (rackboy::resampler::host_resampler_required_inputs(&hr) > 0)
            {
                REQUIRE(pushed < kIn);
                rackboy::resampler::host_resampler_push_input(&hr, inL[pushed], inR[pushed], 0, 0);
                ++pushed;
            }
            rackboy::resampler::host_resampler_emit(&hr, &refL[static_cast<size_t>(i)], &refR[static_cast<size_t>(i)], nullptr, nullptr);
        }

        // Under test: RateResampler in engine-sized chunks.
        RateResampler rs;
        REQUIRE(rs.setRate(sr));
        rs.reset(RateResampler::kHalfTaps);
        std::vector<float> outL(static_cast<size_t>(nOut)), outR(static_cast<size_t>(nOut));
        int used = 0;
        for (int o = 0; o < nOut; o += 512)
        {
            const int n = std::min(512, nOut - o);
            const int need = rs.inputsNeeded(n);
            REQUIRE(used + need <= kIn);
            rs.process(inL.data() + used, inR.data() + used, need, outL.data() + o, outR.data() + o, n);
            used += need;
        }

        double dl = 0.0, dr = 0.0, sig = 0.0;
        for (int i = 0; i < nOut; ++i)
        {
            const double a = static_cast<double>(outL[static_cast<size_t>(i)]) - refL[static_cast<size_t>(i)];
            const double b = static_cast<double>(outR[static_cast<size_t>(i)]) - refR[static_cast<size_t>(i)];
            dl += a * a; dr += b * b;
            sig += static_cast<double>(refL[static_cast<size_t>(i)]) * refL[static_cast<size_t>(i)];
        }
        const double dbL = 10.0 * std::log10(std::max(dl / nOut, 1e-30)), dbR = 10.0 * std::log10(std::max(dr / nOut, 1e-30));
        std::printf("[host rate] RateResampler vs HostResampler at %d: diff RMS L %.1f dBFS, R %.1f dBFS (signal %.1f dBFS)\n", sr,
                    dbL, dbR, 10.0 * std::log10(sig / nOut));
        CHECK(sig / nOut > 0.01);   // not comparing silence
        CHECK(dbL <= -100.0);
        CHECK(dbR <= -100.0);
    }
}

TEST_CASE("host rate: a low note has the same level at 44.1/48/96/192 kHz (DC blocker corner follows the rate)")
{
    double db[4];
    const int rates[4] = {44100, 48000, 96000, 192000};
    for (int k = 0; k < 4; ++k)
    {
        const int sr = rates[k];
        Rig r;
        REQUIRE(r.e->setHostRate(sr));
        r.e->reset();
        r.only(0);
        r.apply();
        r.run(static_cast<long long>(sr) * 2, {noteOn(0, 36)}, 512);
        const double rms = rateRms(r.L, static_cast<size_t>(sr) / 2, static_cast<size_t>(sr) * 2);
        REQUIRE(rms > 0.001);
        db[k] = 20.0 * std::log10(rms);
        std::printf("[host rate] MIDI 36 on Pulse 1 at %d: %.2f dBFS RMS\n", sr, db[k]);
    }
    for (int k = 1; k < 4; ++k)
    {
        CAPTURE(rates[k]);
        CHECK(std::fabs(db[k] - db[0]) <= 0.5);
    }
    // alpha: exactly the literal 0.995f at 44.1 kHz.
    mpcboy::DcBlocker d;
    d.setSampleRate(44100);
    CHECK(d.a == 0.995f);
}

TEST_CASE("host rate: CPU bound, one second of 4-channel render at 192 kHz and 96 kHz")
{
    for (int sr : {96000, 192000})
    {
        CAPTURE(sr);
        auto e = std::make_unique<Engine>();
        REQUIRE(e->setHostRate(sr));
        e->reset();
        EngineSettings s;   // all four channels enabled
        e->applySettings(s);
        std::vector<float> L(512), R(512);
        EngineMidiEvent on;
        on.offset = 0; on.status = 0x90; on.data1 = 48; on.data2 = 100;
        const auto t0 = std::chrono::steady_clock::now();
        for (int c0 = 0; c0 < sr; c0 += 512)
        {
            const int n = std::min(512, sr - c0);
            EngineBlock b;
            b.midi = c0 == 0 ? &on : nullptr;
            b.numMidi = c0 == 0 ? 1 : 0;
            b.bpm = 120.0; b.tempoValid = true;
            e->render(b, L.data(), R.data(), n);
        }
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[host rate] 1 s of render at %d: %.1f ms (%.1f %% of real time)\n", sr, sec * 1000.0, sec * 100.0);
        CHECK(sec < 0.25);
    }
}
