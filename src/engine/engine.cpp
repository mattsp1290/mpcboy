#include "engine.h"

#include <cstring>

#include "engine_consts.h"
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
void fillDefaultWave(uint8_t out[kWaveBytes])
{
    for (int j = 0; j < kWaveBytes; ++j)
    {
        const int a = (2 * j) * 15 / 31;
        const int b = (2 * j + 1) * 15 / 31;
        out[j] = static_cast<uint8_t>((a << 4) | b);
    }
}

// Next event offset inside [c0, c0 + n) of an ascending list, or -1. Offsets
// are clamped to [0, frames - 1]; entries of earlier chunks are skipped by
// advancing `i`. `offsetOf` maps an element to its raw offset.
template <typename T, typename F>
int nextOffset(const T* list, int count, int& i, int frames, int c0, int n, F offsetOf)
{
    while (i < count)
    {
        int o = offsetOf(list[i]);
        o = o < 0 ? 0 : (o > frames - 1 ? frames - 1 : o);
        if (o < c0) { ++i; continue; }
        return o < c0 + n ? o : -1;
    }
    return -1;
}

}  // namespace

Engine::Engine()
{
    std::memset(&apu_, 0, sizeof(apu_));   // reset() below reads the (test-only) write spy before the first init
    rackboy::sav::bank_reset(ownBank_);
    bank_ = &ownBank_;
    // Default saw in every wave slot until content is attached. The slots stay
    // unoccupied, so the table `W` command ignores them.
    for (int i = 0; i < kNumWaves; ++i) fillDefaultWave(ownBank_.waves[i].samples);
    voice_.init(&apu_);
    reset();
}

void Engine::attachContent(ContentStore* store)
{
    content_ = store;
    bank_ = store ? &store->bank() : &ownBank_;
}

void Engine::reset()
{
    // rackboy_apu_init clears the whole struct, including a test's write spy: keep it across a reset.
    const rackboy_apu_write_spy_fn spy = apu_._spy;
    void* const spyUser = apu_._spy_user;
    rackboy_apu_init(&apu_, kApuRate);
    rackboy_apu_set_write_spy(&apu_, spy, spyUser);
    rackboy_apu_set_frame_sequencer(&apu_, 1);         // length / envelope / sweep clocks (MPCBOY LOCAL MOD)
    rackboy_apu_write_reg(&apu_, kNR50, 0x77);    // NR50: both sides at maximum (NR52 = 0x80 was written by init)
    rackboy_apu_write_reg(&apu_, kNR51, 0x00);    // NR51: silent until a note
    rackboy_apu_write_wave_ram(&apu_, bank_->waves[0].samples);   // wave RAM = slot 0
    voice_.reset();
    voice_.applySettings(settings_);
    resampler_.reset(kResamplerPrefill);
    dc_.reset();
    clock_.reset();
    apuFrames_ = 0;
    curHostOffset_ = 0;
    curBlockOffset_ = 0;
    dcResetOut_ = -1;
    chunkOutBase_ = 0;
}

void Engine::applySettings(const EngineSettings& s)
{
    settings_ = s;
    clock_.setSettings(s.tick.mode, s.tick.ticksPerBeat, s.tick.phase, s.tick.freeHz);
    voice_.applySettings(s);
}

void Engine::onNoteTriggered(int blockOffset)
{
    // Tick 0 (row 0) was applied by MonoVoice::noteOn itself; the clock's first
    // tick (row 1) follows one period later. Glides never get here.
    if (clockOn_) clock_.noteOn(blockOffset);
}

void Engine::applyMidi(const EngineMidiEvent& e)
{
    const int st = e.status & 0xF0;
    if (st == 0x90 && e.data2 != 0)
    {
        if (voice_.noteOn(e.data1, e.data2, *bank_))
        {
            onNoteTriggered(curBlockOffset_);
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
    clockOn_ = settings_.tick.enabled;
    if (clockOn_)
    {
        TickBlockInput in;
        in.frames = frames;
        in.bpm = block.bpm;
        in.tempoValid = block.tempoValid;
        in.ppqPos = block.ppqPos;
        in.ppqValid = block.ppqValid;
        in.playing = block.playing;
        clock_.beginBlock(in);
    }
    for (int c0 = 0; c0 < frames; c0 += kMaxHostFrames)
    {
        const int n = frames - c0 < kMaxHostFrames ? frames - c0 : kMaxHostFrames;
        renderChunk(block, frames, c0, n, left + c0, right + c0);
    }
    if (clockOn_) clock_.endBlock();
    clockOn_ = false;
}

// The engine clock's next tick inside [c0, c0 + n), or -1.
int Engine::clockTickIn(int c0, int n) const
{
    const int p = clock_.peek();
    return (p >= c0 && p < c0 + n) ? p : -1;
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
    // skipped by the index scans in nextOffset.
    while (true)
    {
        // Two streams: MIDI, and ticks from exactly one source (the clock, or
        // EngineBlock::tickOffsets when the clock is off). MIDI wins ties.
        const int mOff = nextOffset(block.midi, block.numMidi, mi, frames, c0, n,
                                    [](const EngineMidiEvent& e) { return e.offset; });
        const int tOff = clockOn_ ? clockTickIn(c0, n)
                                  : nextOffset(block.tickOffsets, block.numTicks, ti, frames, c0, n,
                                               [](int o) { return o; });
        if (mOff < 0 && tOff < 0) break;

        const bool takeMidi = (mOff >= 0) && (tOff < 0 || mOff <= tOff);
        const int evOff = takeMidi ? mOff : tOff;
        const int t = evOff - c0;
        // Absolute mapping: APU frame round((outputsSoFar + t) * 160 / 147) minus
        // the APU frames already rendered before this chunk. Rounding once from
        // the absolute position keeps the error below half an APU frame instead
        // of letting the per-block resampler granularity (+-1 APU frame) add up.
        const long long outAbs = chunkOutBase_ + t;
        int a = static_cast<int>((outAbs * 2 * 160 + 147) / (2 * 147) - static_cast<long long>(apuBase));
        if (a < apuPos) a = apuPos;
        runTo(a);
        curHostOffset_ = t;
        curBlockOffset_ = evOff;
        if (takeMidi) { applyMidi(block.midi[mi]); ++mi; }
        else
        {
            voice_.tick(*bank_);
            if (clockOn_) clock_.pop(); else ++ti;
        }
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
}

}  // namespace mpcboy
