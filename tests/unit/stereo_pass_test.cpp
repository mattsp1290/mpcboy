// WP3 tests 1-3: single-pass stereo APU (rackboy_apu_run_stereo).
//
// Test 1 replays tests/rackboy/audio/pattern.csv exactly as the golden
// regression does (same fixture, trigger_all / tick_all, one APU sample per
// step) but only to record a register-write log through the test-only spy; the
// log is then replayed into rackboy_apu_run_stereo and compared with the sum of
// the four golden per-channel buffers. Must be run with cwd = tests/rackboy
// (ctest does that).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "doctest/doctest.h"

extern "C" {
#include "rackboy_apu.h"
}

#include "common/fixture_loader.h"
#include "dc_blocker.h"
#include "rackboy_bank.h"
#include "rackboy_clock.h"
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_sidecar.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_validate.h"
#include "rackboy_table.h"
#include "rackboy_trigger.h"
#include "rackboy_voice.h"

namespace {

constexpr std::size_t kSamples = 96000;   // 2 s at 48 kHz (AD #11)
constexpr int kCh = 4;

std::string try_open(const std::vector<std::string>& candidates)
{
    for (const auto& p : candidates)
    {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (f) { std::fclose(f); return p; }
    }
    return std::string();
}

struct Ev
{
    std::size_t sample;
    int type;               // 0 = gate1_rising, 1 = gate2_rising, 2 = other (ignored)
    std::int16_t slots[kCh];
};

std::vector<Ev> read_pattern(const std::string& path)
{
    std::ifstream in(path);
    REQUIRE_MESSAGE(in, "cannot open " << path);
    std::vector<Ev> evs;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        const std::size_t c1 = line.find(',');
        const std::size_t c2 = line.find(',', c1 + 1);
        REQUIRE(c1 != std::string::npos);
        Ev e{};
        e.sample = static_cast<std::size_t>(std::nearbyint(std::stod(line.substr(0, c1)) * 48000.0));
        std::string type = line.substr(c1 + 1, (c2 == std::string::npos ? line.size() : c2) - c1 - 1);
        type.erase(0, type.find_first_not_of(" \t"));
        type.erase(type.find_last_not_of(" \t") + 1);
        if (type == "gate1_rising")
        {
            e.type = 0;
            REQUIRE(c2 != std::string::npos);
            std::string v = line.substr(c2 + 1);
            std::string digits;
            for (char ch : v) if (ch != '"' && ch != ' ') digits += ch;
            std::stringstream ss(digits);
            std::string tok;
            int i = 0;
            while (std::getline(ss, tok, ',') && i < kCh) e.slots[i++] = static_cast<std::int16_t>(std::stoi(tok));
            REQUIRE(i == kCh);
        }
        else if (type == "gate2_rising") e.type = 1;
        else if (type == "gate1_falling" || type == "gate2_falling") e.type = 2;
        else FAIL("unsupported pattern event: " << type);
        evs.push_back(e);
    }
    REQUIRE(!evs.empty());
    return evs;
}

rackboy::sav::RackboyBank load_fixture_bank()
{
    namespace fx = rackboy::tests::common;
    const std::vector<std::uint8_t> sav = fx::load_fixture_bytes();
    const std::size_t start = rackboy::sav::first_block_for_slot(sav.data(), rackboy::sav::kRackboyV1ActiveSlot);
    REQUIRE(start < rackboy::sav::kBlockAllocTableLen);
    std::string err;
    auto song = rackboy::sav::compression::decompress(sav.data(), sav.size(), static_cast<std::uint8_t>(start), &err);
    REQUIRE_MESSAGE(song.has_value(), err);
    rackboy::sav::LoadInputs li{};
    li.raw_sav = sav.data();
    li.raw_sav_len = sav.size();
    li.decompressed_song = song->data();
    li.decompressed_song_len = song->size();
    rackboy::sav::RackboyBank bank{};
    rackboy::sav::RackboyBankNames names{};
    const auto r = rackboy::sav::load_rackboy_sav(li, &bank, &names);
    REQUIRE(r.phase == rackboy::sav::LoadPhase::Ok);
    return bank;
}

struct WriteRec { std::size_t sample; uint16_t addr; uint8_t value; };

struct SpyCtx
{
    std::vector<WriteRec>* log;
    std::size_t sample;
};

void spy_cb(void* user, uint16_t addr, uint8_t value)
{
    auto* c = static_cast<SpyCtx*>(user);
    c->log->push_back({c->sample, addr, value});
}

bool read_bin(const std::string& path, std::vector<float>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.resize(kSamples);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(kSamples * sizeof(float)));
    if (static_cast<std::size_t>(in.gcount()) != kSamples * sizeof(float)) return false;
    char extra;
    in.read(&extra, 1);
    return in.gcount() == 0;
}

double rms_dbfs(const std::vector<float>& v, std::size_t from = 0, std::size_t to = SIZE_MAX)
{
    if (to > v.size()) to = v.size();
    double acc = 0.0;
    for (std::size_t i = from; i < to; ++i) acc += static_cast<double>(v[i]) * v[i];
    const double rms = std::sqrt(acc / static_cast<double>(to - from));
    return rms == 0.0 ? -300.0 : 20.0 * std::log10(rms);
}

void trigger_pulse1(rackboy_apu_t* apu, uint16_t freq, uint8_t nr51)
{
    rackboy_apu_write_reg(apu, 0xFF24, 0x77);
    rackboy_apu_write_reg(apu, 0xFF25, nr51);
    rackboy_apu_write_reg(apu, 0xFF11, 0x80);   // 50 % duty
    rackboy_apu_write_reg(apu, 0xFF12, 0xF0);   // volume 15, no envelope
    rackboy_apu_write_reg(apu, 0xFF13, static_cast<uint8_t>(freq & 0xFF));
    rackboy_apu_write_reg(apu, 0xFF14, static_cast<uint8_t>(0x80 | ((freq >> 8) & 7)));
}

}  // namespace

TEST_CASE("stereo pass 1: run_stereo replay equals the sum of the four golden channels (R1)")
{
    const std::string pattern_path = try_open({"audio/pattern.csv", "tests/rackboy/audio/pattern.csv"});
    REQUIRE_MESSAGE(!pattern_path.empty(), "audio/pattern.csv not found (cwd must be tests/rackboy)");
    static const char* const names[kCh] = {"pulse1.bin", "pulse2.bin", "wave.bin", "noise.bin"};
    std::vector<float> golden[kCh];
    for (int c = 0; c < kCh; ++c)
    {
        const std::string p = try_open({std::string("audio/reference/") + names[c],
                                        std::string("tests/rackboy/audio/reference/") + names[c]});
        REQUIRE_MESSAGE(!p.empty(), "golden " << names[c] << " missing: this test fails, it does not skip");
        REQUIRE_MESSAGE(read_bin(p, golden[c]), "cannot read golden " << p);
    }

    const auto pattern = read_pattern(pattern_path);
    const auto bank = load_fixture_bank();

    // (a) Record the register writes exactly as regression.cpp produces them.
    std::vector<WriteRec> log;
    {
        rackboy_apu_t apu{};
        std::memset(&apu, 0, sizeof(apu));
        rackboy_apu_init(&apu, 48000);
        rackboy::core::VoiceState voices[rackboy::core::kNumVoices];
        rackboy::core::voices_reset_all(voices);
        rackboy::table::TableRegState regs[rackboy::core::kNumVoices];
        for (auto& r : regs) rackboy::table::table_reg_state_reset(r);

        SpyCtx ctx{&log, 0};
        rackboy_apu_set_write_spy(spy_cb, &ctx);
        std::size_t ei = 0;
        for (std::size_t s = 0; s < kSamples; ++s)
        {
            ctx.sample = s;
            while (ei < pattern.size() && pattern[ei].sample <= s)
            {
                const Ev& e = pattern[ei++];
                if (e.type == 0)
                {
                    const std::int16_t pool[kCh] = {static_cast<std::int16_t>(bank.pulse1_count),
                                                    static_cast<std::int16_t>(bank.pulse2_count),
                                                    static_cast<std::int16_t>(bank.wave_count),
                                                    static_cast<std::int16_t>(bank.noise_count)};
                    for (int c = 0; c < kCh; ++c)
                        voices[c].active_instrument_index =
                            (e.slots[c] >= 0 && e.slots[c] < pool[c]) ? e.slots[c] : rackboy::core::kNoInstrument;
                    rackboy::core::trigger_all(voices, regs, bank, &apu);
                }
                else if (e.type == 1)
                {
                    rackboy::core::tick_all(voices, regs, bank, &apu);
                }
            }
        }
        rackboy_apu_set_write_spy(nullptr, nullptr);
    }
    REQUIRE(log.size() > 20);

    // (b) Replay into one stereo pass: NR51 = 0xFF, NR50 = 0x77, writes applied
    // before the sample they were recorded at.
    std::vector<float> L(kSamples), R(kSamples);
    {
        rackboy_apu_t apu{};
        std::memset(&apu, 0, sizeof(apu));
        rackboy_apu_init(&apu, 48000);
        rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
        rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
        std::size_t wi = 0;
        for (std::size_t s = 0; s < kSamples; ++s)
        {
            while (wi < log.size() && log[wi].sample <= s)
            {
                rackboy_apu_write_reg(&apu, log[wi].addr, log[wi].value);
                ++wi;
            }
            rackboy_apu_run_stereo(&apu, 1, &L[s], &R[s]);
        }
    }

    // (c) Same DC blocker as taps_process.
    mpcboy::DcBlocker dc;
    dc.process(L.data(), R.data(), static_cast<int>(kSamples));

    // (d) Compare with the golden sum, scale fixed at 1.0.
    std::vector<float> dl(kSamples), dr(kSamples);
    double goldenRms = 0.0;
    for (std::size_t i = 0; i < kSamples; ++i)
    {
        const float sum = golden[0][i] + golden[1][i] + golden[2][i] + golden[3][i];
        dl[i] = L[i] - sum;
        dr[i] = R[i] - sum;
        goldenRms += static_cast<double>(sum) * sum;
    }
    const double dbL = rms_dbfs(dl), dbR = rms_dbfs(dr);
    std::printf("[stereo equivalence] diff RMS: left %.2f dBFS, right %.2f dBFS (golden sum RMS %.2f dBFS)\n",
                dbL, dbR, 10.0 * std::log10(goldenRms / kSamples));
    CHECK(dbL < -60.0);
    CHECK(dbR < -60.0);
    CHECK(goldenRms > 0.0);   // the comparison is not trivially silent
}

TEST_CASE("stereo pass 2: one call equals 344 calls of varying size, sample for sample")
{
    auto setup = [](rackboy_apu_t& apu) {
        std::memset(&apu, 0, sizeof(apu));
        rackboy_apu_init(&apu, 48000);
        rackboy_apu_write_reg(&apu, 0xFF24, 0x77);
        rackboy_apu_write_reg(&apu, 0xFF25, 0xFF);
        // Pulse 1 + Pulse 2 + Wave + Noise all sounding.
        rackboy_apu_write_reg(&apu, 0xFF11, 0x80); rackboy_apu_write_reg(&apu, 0xFF12, 0xF1);
        rackboy_apu_write_reg(&apu, 0xFF13, 0x00); rackboy_apu_write_reg(&apu, 0xFF14, 0x86);
        rackboy_apu_write_reg(&apu, 0xFF16, 0x40); rackboy_apu_write_reg(&apu, 0xFF17, 0xC2);
        rackboy_apu_write_reg(&apu, 0xFF18, 0x80); rackboy_apu_write_reg(&apu, 0xFF19, 0x85);
        uint8_t wave[16];
        for (int i = 0; i < 16; ++i) wave[i] = static_cast<uint8_t>((i * 16 + 15 - i) & 0xFF);
        rackboy_apu_write_wave_ram(&apu, wave);
        rackboy_apu_write_reg(&apu, 0xFF1A, 0x80); rackboy_apu_write_reg(&apu, 0xFF1C, 0x20);
        rackboy_apu_write_reg(&apu, 0xFF1D, 0x00); rackboy_apu_write_reg(&apu, 0xFF1E, 0x84);
        rackboy_apu_write_reg(&apu, 0xFF21, 0xF1); rackboy_apu_write_reg(&apu, 0xFF22, 0x33);
        rackboy_apu_write_reg(&apu, 0xFF23, 0x80);
    };
    constexpr int kTotal = 48000;
    std::vector<float> aL(kTotal), aR(kTotal), bL(kTotal), bR(kTotal);
    static rackboy_apu_t apuA, apuB;
    setup(apuA);
    rackboy_apu_run_stereo(&apuA, kTotal, aL.data(), aR.data());
    setup(apuB);
    uint32_t seed = 12345;
    int pos = 0, calls = 0;
    while (pos < kTotal)
    {
        seed = seed * 1664525u + 1013904223u;
        int n = 32 + static_cast<int>((seed >> 16) % 481);   // 32..512
        if (n > kTotal - pos) n = kTotal - pos;
        rackboy_apu_run_stereo(&apuB, static_cast<uint32_t>(n), bL.data() + pos, bR.data() + pos);
        pos += n;
        ++calls;
    }
    CHECK(calls > 100);
    CHECK(std::memcmp(aL.data(), bL.data(), sizeof(float) * kTotal) == 0);
    CHECK(std::memcmp(aR.data(), bR.data(), sizeof(float) * kTotal) == 0);
    CHECK(rms_dbfs(aL) > -40.0);   // the stream is not silent
}

TEST_CASE("stereo pass 3: NR51 = 0x10 puts Pulse 1 on the left only")
{
    static rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    trigger_pulse1(&apu, 1900, 0x10);
    std::vector<float> L(24000), R(24000);
    rackboy_apu_run_stereo(&apu, 24000, L.data(), R.data());
    const double dbL = rms_dbfs(L, 1000), dbR = rms_dbfs(R, 1000);
    std::printf("[stereo pan] left %.2f dBFS, right %.2f dBFS\n", dbL, dbR);
    CHECK(dbR < -90.0);
    CHECK(dbL > -30.0);
}

TEST_CASE("stereo pass: run_samples still works after run_stereo (callback restored)")
{
    static rackboy_apu_t apu;
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, 48000);
    trigger_pulse1(&apu, 1900, 0xFF);
    std::vector<float> L(2000), R(2000), c1(2000), c2(2000), c3(2000), c4(2000);
    rackboy_apu_run_stereo(&apu, 2000, L.data(), R.data());
    rackboy_apu_run_samples(&apu, 2000, c1.data(), c2.data(), c3.data(), c4.data());
    CHECK(rms_dbfs(c1) > -30.0);
    CHECK(rms_dbfs(c2) < -90.0);
}
