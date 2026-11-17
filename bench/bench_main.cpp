// mpcboy_bench - native CPU benchmark (WP3 test 6).
//
// Workload A (the WP3 pass bar): rackboy_apu_run_stereo for all four channels
// sounding + PolyphaseResampler48To441, in 128-frame host blocks (the APU
// renders inputsNeeded(128) = 136..141 frames per block).
// Workload B (informational): the same through mpcboy::Engine::render with a
// held note, four channels and four tables ticking (24 ticks / beat at 120 BPM).
//
// Prints mean / p50 / p99 / p99.9 / max in microseconds per 128-frame block.
// Exit status 1 when workload A's p99 exceeds the 25 us pass bar.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include "rackboy_apu.h"
}

#include "engine.h"
#include "polyphase_resampler.h"

namespace {

constexpr int kBlock = 128;
constexpr int kWarmup = 4000;
constexpr int kBlocks = 200000;
constexpr double kPassBarP99Us = 25.0;

using Clock = std::chrono::steady_clock;

struct Stats { double mean, p50, p99, p999, max; };

Stats summarize(std::vector<double>& us)
{
    std::sort(us.begin(), us.end());
    double sum = 0;
    for (double v : us) sum += v;
    auto at = [&](double q) { return us[static_cast<size_t>(q * static_cast<double>(us.size() - 1))]; };
    return {sum / static_cast<double>(us.size()), at(0.5), at(0.99), at(0.999), us.back()};
}

void print(const char* name, const Stats& s)
{
    std::printf("%-44s mean %7.2f  p50 %7.2f  p99 %7.2f  p99.9 %7.2f  max %8.2f  (us / %d-frame block)\n",
                name, s.mean, s.p50, s.p99, s.p999, s.max, kBlock);
}

void setupFourChannels(rackboy_apu_t* apu)
{
    rackboy_apu_write_reg(apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(apu, 0xFF25, 0xFF);
    rackboy_apu_write_reg(apu, 0xFF11, 0x80); rackboy_apu_write_reg(apu, 0xFF12, 0xF0);
    rackboy_apu_write_reg(apu, 0xFF13, 0x00); rackboy_apu_write_reg(apu, 0xFF14, 0x86);
    rackboy_apu_write_reg(apu, 0xFF16, 0x40); rackboy_apu_write_reg(apu, 0xFF17, 0xF0);
    rackboy_apu_write_reg(apu, 0xFF18, 0x80); rackboy_apu_write_reg(apu, 0xFF19, 0x85);
    uint8_t wave[16];
    for (int i = 0; i < 16; ++i) wave[i] = static_cast<uint8_t>(((2 * i * 15 / 31) << 4) | ((2 * i + 1) * 15 / 31));
    rackboy_apu_write_wave_ram(apu, wave);
    rackboy_apu_write_reg(apu, 0xFF1A, 0x80); rackboy_apu_write_reg(apu, 0xFF1C, 0x20);
    rackboy_apu_write_reg(apu, 0xFF1D, 0x00); rackboy_apu_write_reg(apu, 0xFF1E, 0x84);
    rackboy_apu_write_reg(apu, 0xFF21, 0xF0); rackboy_apu_write_reg(apu, 0xFF22, 0x33);
    rackboy_apu_write_reg(apu, 0xFF23, 0x80);
}

Stats benchApuAndResampler()
{
    auto apu = std::make_unique<rackboy_apu_t>();
    std::memset(apu.get(), 0, sizeof(*apu));
    rackboy_apu_init(apu.get(), 48000);
    setupFourChannels(apu.get());
    auto rs = std::make_unique<mpcboy::PolyphaseResampler48To441>();
    rs->reset(mpcboy::PolyphaseResampler48To441::kHalfTaps);

    float aL[1024], aR[1024], oL[kBlock], oR[kBlock];
    std::vector<double> us;
    us.reserve(kBlocks);
    volatile float sink = 0.0f;
    for (int b = 0; b < kWarmup + kBlocks; ++b)
    {
        const auto t0 = Clock::now();
        const int need = rs->inputsNeeded(kBlock);
        rackboy_apu_run_stereo(apu.get(), static_cast<uint32_t>(need), aL, aR);
        rs->process(aL, aR, need, oL, oR, kBlock);
        const auto t1 = Clock::now();
        sink = sink + oL[7] + oR[91];
        if (b >= kWarmup) us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(us);
}

Stats benchApuOnly()
{
    auto apu = std::make_unique<rackboy_apu_t>();
    std::memset(apu.get(), 0, sizeof(*apu));
    rackboy_apu_init(apu.get(), 48000);
    setupFourChannels(apu.get());
    float aL[1024], aR[1024];
    std::vector<double> us;
    us.reserve(kBlocks);
    volatile float sink = 0.0f;
    for (int b = 0; b < kWarmup + kBlocks; ++b)
    {
        const int need = 136 + (b % 5 == 0 ? 0 : 1) + (b % 7 == 0 ? 1 : 0);   // 136..138, averaging ~137
        const auto t0 = Clock::now();
        rackboy_apu_run_stereo(apu.get(), static_cast<uint32_t>(need), aL, aR);
        const auto t1 = Clock::now();
        sink = sink + aL[5];
        if (b >= kWarmup) us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(us);
}

Stats benchResamplerOnly()
{
    auto rs = std::make_unique<mpcboy::PolyphaseResampler48To441>();
    rs->reset(mpcboy::PolyphaseResampler48To441::kHalfTaps);
    std::vector<float> in(1024);
    for (size_t i = 0; i < in.size(); ++i) in[i] = ((i * 37) % 64) / 64.0f - 0.5f;
    float oL[kBlock], oR[kBlock];
    std::vector<double> us;
    us.reserve(kBlocks);
    volatile float sink = 0.0f;
    for (int b = 0; b < kWarmup + kBlocks; ++b)
    {
        const int need = rs->inputsNeeded(kBlock);
        const auto t0 = Clock::now();
        rs->process(in.data(), in.data(), need, oL, oR, kBlock);
        const auto t1 = Clock::now();
        sink = sink + oL[3];
        if (b >= kWarmup) us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(us);
}

Stats benchEngine()
{
    auto eng = std::make_unique<mpcboy::Engine>();
    // Four tables (each channel its own, arpeggio-like) in the engine's own bank.
    auto& bank = eng->ownBank();
    for (int t = 0; t < 4; ++t)
    {
        bank.tables[t].occupied = true;
        // Arpeggio that returns to its start (rows 1..3 sum to 0): +0, +(3+t), +(4-t), -7 H01.
        const int8_t steps[4] = {0, static_cast<int8_t>(3 + t), static_cast<int8_t>(4 - t), -7};
        for (int r = 0; r < 4; ++r) bank.tables[t].rows[r].transpose = steps[r];
        bank.tables[t].rows[3].cmd1 = 'H';
        bank.tables[t].rows[3].cmd1_param = 1;
    }
    mpcboy::EngineSettings s;
    for (int c = 0; c < 4; ++c) { s.ch[c].tableSlot = static_cast<int8_t>(c); s.ch[c].volume = 15; }
    eng->applySettings(s);

    mpcboy::EngineMidiEvent on;
    on.offset = 0; on.status = 0x90; on.data1 = 60; on.data2 = 100;
    float L[kBlock], R[kBlock];
    std::vector<double> us;
    us.reserve(kBlocks);
    volatile float sink = 0.0f;
    double nextTick = 918.75;   // 24 ticks/beat @ 120 BPM, host frames
    double blockStart = 0.0;
    for (int b = 0; b < kWarmup + kBlocks; ++b)
    {
        mpcboy::EngineBlock blk;
        if (b == 0) { blk.midi = &on; blk.numMidi = 1; }
        int ticks[8];
        int nt = 0;
        while (nextTick < blockStart + kBlock && nt < 8)
        {
            ticks[nt++] = static_cast<int>(nextTick - blockStart);
            nextTick += 918.75;
        }
        blk.tickOffsets = ticks;
        blk.numTicks = nt;
        const auto t0 = Clock::now();
        eng->render(blk, L, R, kBlock);
        const auto t1 = Clock::now();
        sink = sink + L[11] + R[13];
        blockStart += kBlock;
        if (b >= kWarmup) us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return summarize(us);
}

}  // namespace

int main()
{
    std::printf("mpcboy_bench: %d blocks of %d frames after %d warm-up, 4 channels sounding\n", kBlocks, kBlock, kWarmup);
    const Stats apuOnly = benchApuOnly();
    const Stats rsOnly = benchResamplerOnly();
    const Stats both = benchApuAndResampler();
    const Stats engine = benchEngine();
    print("APU single pass only (~137 frames)", apuOnly);
    print("resampler only", rsOnly);
    print("A: APU single pass + resampler", both);
    print("B: Engine::render (4 ch, 4 tables, ticks)", engine);
    std::printf("device budget per block: mean <= 145 us, p99 <= 290 us (S8); native pass bar: A p99 <= %.0f us\n",
                kPassBarP99Us);
    const bool pass = both.p99 <= kPassBarP99Us;
    std::printf("native gate (A p99 <= %.0f us): %s\n", kPassBarP99Us, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
