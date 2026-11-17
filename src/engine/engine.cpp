#include "engine.h"

#include <cstring>

#include "rackboy_table.h"

namespace mpcboy {

namespace {

// A Cut note-off turns the DACs off at APU frame a. The resampler's output
// stops depending on that step ~237 host frames later (kernel half width 128
// APU frames each side). Past that point only the DC blocker's tail is left: a
// one-pole high-pass with alpha 0.995 turns the DAC's DC step into a
// -47 dBFS exponential that needs ~26 ms to fall below -90 dBFS. The engine
// therefore flushes the blocker at that moment (the step it removes is the tail
// itself, ~-47 dBFS), which gives the plan's "silent within 10 ms + resampler
// delay" after a note-off.
constexpr int kDcFlushDelay = 240;

// Default wave for every slot until content is loaded: a 4-bit saw.
void fillDefaultWave(uint8_t out[16])
{
    for (int j = 0; j < 16; ++j)
    {
        const int a = (2 * j) * 15 / 31;
        const int b = (2 * j + 1) * 15 / 31;
        out[j] = static_cast<uint8_t>((a << 4) | b);
    }
}

}  // namespace

Engine::Engine()
{
    rackboy::sav::bank_reset(ownBank_);
    bank_ = &ownBank_;
    for (int i = 0; i < 16; ++i) fillDefaultWave(waves_[i]);
    voice_.init(&apu_, waves_);
    reset();
}

void Engine::setWave(int slot, const uint8_t bytes[16])
{
    if (slot < 0 || slot > 15) return;
    std::memcpy(waves_[slot], bytes, 16);
}

void Engine::reset()
{
    rackboy_apu_init(&apu_, kApuRate);                 // also writes NR52 = 0x80
    rackboy_apu_set_frame_sequencer(&apu_, 1);         // length / envelope / sweep clocks (MPCBOY LOCAL MOD)
    rackboy_apu_write_reg(&apu_, 0xFF26, 0x80);        // NR52: APU on
    rackboy_apu_write_reg(&apu_, 0xFF24, 0x77);        // NR50: both sides at maximum
    rackboy_apu_write_reg(&apu_, 0xFF25, 0x00);        // NR51: silent until a note
    rackboy_apu_write_wave_ram(&apu_, waves_[0]);      // wave RAM = slot 0
    voice_.reset();
    voice_.applySettings(settings_);
    resampler_.reset(kResamplerPrefill);
    dc_.reset();
    apuFrames_ = 0;
    curHostOffset_ = 0;
    dcResetOut_ = -1;
    chunkOutBase_ = 0;
}

void Engine::applySettings(const EngineSettings& s)
{
    settings_ = s;
    voice_.applySettings(s);
}

void Engine::onNoteTriggered(int hostOffset)
{
    (void)hostOffset;   // WP6: tickClock_.noteOn(hostOffset)
}

void Engine::applyMidi(const EngineMidiEvent& e)
{
    const int st = e.status & 0xF0;
    if (st == 0x90 && e.data2 != 0)
    {
        if (voice_.noteOn(e.data1, e.data2, *bank_))
        {
            onNoteTriggered(curHostOffset_);
            dcResetOut_ = -1;   // new sound: do not flush it
        }
    }
    else if (st == 0x80 || st == 0x90)
    {
        voice_.noteOff(e.data1);
    }
    else if (st == 0xE0)
    {
        voice_.pitchBend((e.data1 | (e.data2 << 7)) - 8192);
    }
    else if (st == 0xB0 && (e.data1 == 120 || e.data1 == 123))
    {
        voice_.allNotesOff();
    }
    if (voice_.takeSilenced()) dcResetOut_ = chunkOutBase_ + curHostOffset_ + kDcFlushDelay;
}

void Engine::render(const EngineBlock& block, float* left, float* right, int frames)
{
    for (int c0 = 0; c0 < frames; c0 += kMaxHostFrames)
    {
        const int n = frames - c0 < kMaxHostFrames ? frames - c0 : kMaxHostFrames;
        renderChunk(block, frames, c0, n, left + c0, right + c0);
    }
}

void Engine::renderChunk(const EngineBlock& block, int frames, int c0, int n, float* left, float* right)
{
    // APU frames for this chunk: exactly what the resampler needs to emit n
    // host frames. Host offset t maps to APU frame round(t * 160 / 147); no
    // lookahead (the resampler's group delay delays when a change is heard,
    // not when it is written).
    const int nIn = resampler_.inputsNeeded(n);
    const uint64_t apuBase = apuFrames_;
    chunkOutBase_ = resampler_.outputCount();
    int apuPos = 0;

    auto runTo = [&](int target) {
        if (target > nIn) target = nIn;
        if (target > apuPos)
        {
            rackboy_apu_run_stereo(&apu_, static_cast<uint32_t>(target - apuPos),
                                   apuL_ + apuPos, apuR_ + apuPos);
            apuPos = target;
        }
        apuFrames_ = apuBase + static_cast<uint64_t>(apuPos);
    };

    int mi = 0, ti = 0;
    // Offsets are relative to the whole host block; out-of-range ones clamp to
    // [0, frames - 1] (the SDK does the same). Events of earlier chunks are
    // skipped by the index scans below.
    while (true)
    {
        // Find the next event inside this chunk: MIDI before tick at equal offsets.
        int mOff = -1, tOff = -1;
        while (mi < block.numMidi)
        {
            int o = block.midi[mi].offset;
            o = o < 0 ? 0 : (o > frames - 1 ? frames - 1 : o);
            if (o < c0) { ++mi; continue; }
            if (o >= c0 + n) { mOff = -1; break; }
            mOff = o;
            break;
        }
        while (ti < block.numTicks)
        {
            int o = block.tickOffsets[ti];
            o = o < 0 ? 0 : (o > frames - 1 ? frames - 1 : o);
            if (o < c0) { ++ti; continue; }
            if (o >= c0 + n) { tOff = -1; break; }
            tOff = o;
            break;
        }
        if (mOff < 0 && tOff < 0) break;

        const bool takeMidi = (mOff >= 0) && (tOff < 0 || mOff <= tOff);
        const int t = (takeMidi ? mOff : tOff) - c0;
        int a = (t * 2 * 160 + 147) / (2 * 147);   // round(t * 160 / 147)
        if (a < apuPos) a = apuPos;
        runTo(a);
        curHostOffset_ = t;
        if (takeMidi) { applyMidi(block.midi[mi]); ++mi; }
        else { voice_.tick(*bank_); ++ti; }
    }
    runTo(nIn);

    resampler_.process(apuL_, apuR_, nIn, left, right, n);
    if (dcResetOut_ >= 0 && dcResetOut_ - chunkOutBase_ < n)
    {
        const int k = dcResetOut_ > chunkOutBase_ ? static_cast<int>(dcResetOut_ - chunkOutBase_) : 0;
        dc_.process(left, right, k);
        dc_.reset();
        dc_.process(left + k, right + k, n - k);
        dcResetOut_ = -1;
    }
    else
    {
        dc_.process(left, right, n);
    }
    const float g = settings_.masterGain;
    if (g != 1.0f)
        for (int i = 0; i < n; ++i) { left[i] *= g; right[i] *= g; }
}

}  // namespace mpcboy
