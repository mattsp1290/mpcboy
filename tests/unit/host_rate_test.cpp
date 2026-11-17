// Host sample rate support: tick timing, audio sanity, large blocks, rate changes
// through the plugin, unsupported rates. 44.1 kHz bit-identity is covered by the
// existing suites (golden regression, engine tests) which all run at 44.1 kHz.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "doctest/doctest.h"
#include "engine_test_support.h"
#include "plugin_test_support.h"

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
