// src/engine/engine.h - host-agnostic synth engine (WP4).
//
//   MIDI events + tick offsets (host frames, 44.1 kHz)
//     -> MonoVoice -> 4 channels -> one SameBoy APU pass at 48 kHz (stereo)
//     -> PolyphaseResampler48To441 -> DC blocker -> master gain.
//
// Everything is audio-thread only. render() allocates nothing and takes no
// lock. No SDK headers: the plugin adapts mpc::MidiEvent / Transport to
// EngineBlock.
#pragma once

#include <cstdint>

#include "channel_settings.h"
#include "dc_blocker.h"
#include "mono_voice.h"
#include "polyphase_resampler.h"
#include "rackboy_apu.h"
#include "rackboy_sav_types.h"

namespace mpcboy {

struct EngineMidiEvent
{
    int offset = 0;          // host frames from the start of the block
    uint8_t status = 0;      // raw status byte (channel nibble ignored)
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// One host block of input. MIDI offsets must be ascending.
struct EngineBlock
{
    const EngineMidiEvent* midi = nullptr;
    int numMidi = 0;

    // WP6 seam: table tick positions in host frames from the start of the
    // block, ascending. WP6's TickClock fills these; until then the caller (or
    // nothing) does. At equal offsets MIDI is applied before the tick.
    const int* tickOffsets = nullptr;
    int numTicks = 0;

    // Transport fields for WP6's TickClock (unused by Engine::render itself).
    double bpm = 120.0;
    bool tempoValid = false;
    double ppqPos = 0.0;
    bool ppqValid = false;
    bool playing = false;
};

class Engine
{
public:
    static constexpr int kMaxHostFrames = 512;
    // 512 host frames need at most 559 APU frames once the resampler is primed.
    static constexpr int kMaxApuFrames = 640;
    static constexpr int kApuRate = 48000;
    static constexpr int kResamplerPrefill = PolyphaseResampler48To441::kHalfTaps;

    // Large object (it embeds the SameBoy APU state): create it on the heap.
    Engine();

    // Audio-thread only. Init sequence of 02-apu-and-voice.md: APU init,
    // NR52 = 0x80, NR50 = 0x77, NR51 = 0x00, wave RAM = slot 0; clears the note
    // stack, voices and bend; resets the resampler (primed) and DC blocker.
    void reset();

    void applySettings(const EngineSettings& s);

    // Table / wave content the voice reads. The engine starts with an empty
    // bank of its own (no tables) and 16 default wave slots.
    void setBank(const rackboy::sav::RackboyBank* bank) { bank_ = bank ? bank : &ownBank_; }
    rackboy::sav::RackboyBank& ownBank() { return ownBank_; }
    void setWave(int slot, const uint8_t bytes[16]);

    void render(const EngineBlock& block, float* left, float* right, int frames);

    // APU frames rendered since reset (the spy in tests stamps register writes
    // with this).
    uint64_t apuFrame() const { return apuFrames_; }
    rackboy_apu_t* apu() { return &apu_; }
    const MonoVoice& voice() const { return voice_; }

private:
    void renderChunk(const EngineBlock& block, int frames, int c0, int n, float* left, float* right);
    void applyMidi(const EngineMidiEvent& e);
    void onNoteTriggered(int hostOffset);   // WP6 hook: restart the tick clock's note phase

    rackboy_apu_t apu_;
    MonoVoice voice_;
    PolyphaseResampler48To441 resampler_;
    DcBlocker dc_;
    EngineSettings settings_;
    rackboy::sav::RackboyBank ownBank_;
    const rackboy::sav::RackboyBank* bank_;
    uint8_t waves_[16][16];
    uint64_t apuFrames_ = 0;
    int curHostOffset_ = 0;
    // Output frame (absolute) at which the DC blocker is flushed after a Cut,
    // or -1. See renderChunk.
    long long dcResetOut_ = -1;
    long long chunkOutBase_ = 0;
    float apuL_[kMaxApuFrames];
    float apuR_[kMaxApuFrames];
};

}  // namespace mpcboy
