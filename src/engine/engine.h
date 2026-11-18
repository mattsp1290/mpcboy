// src/engine/engine.h - host-agnostic synth engine (WP4).
//
//   MIDI events + tick offsets (host frames, at the host rate)
//     -> MonoVoice -> 4 channels -> one SameBoy APU pass at 48 kHz (stereo)
//     -> host-rate conversion -> DC blocker -> master gain.
//
// Host rates (setHostRate): 44100 uses PolyphaseResampler48To441 (the MPC path,
// table driven, bit-identical to before rate support); 48000 passes the APU
// output straight through (no resampling, no latency); 96000 and 192000 use
// RateResampler (same kernel as the vendored HostResampler, table driven with
// L = 2 / 4 phases; a desktop-only path, the MPC never selects it). Any other
// rate is unsupported: setHostRate returns false and the caller must not render.
//
// Output delay by rate (resampler group delay, 128 APU frames = 2.67 ms): 44.1k
// ~117.6 host frames, 48k 0, 96k 256, 192k 512 (+ about 30 APU frames of APU
// register latency at every rate). The plugin still reports latency 0.
//
// Everything is audio-thread only. render() allocates nothing and takes no
// lock. No SDK headers: the plugin adapts mpc::MidiEvent / Transport to
// EngineBlock.
#pragma once

#include <cstdint>

#include "channel_settings.h"
#include "content_store.h"
#include "dc_blocker.h"
#include "mono_voice.h"
#include "playback_state.h"
#include "polyphase_resampler.h"
#include "rackboy_apu.h"
#include "rate_resampler.h"
#include "rackboy_sav_types.h"
#include "tick_clock.h"

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

    // External table tick positions in host frames from the start of the
    // block, ascending (tests, the legacy CPU probe). Read only when the
    // engine's own TickClock is off (EngineSettings::tick.enabled == false);
    // with the clock on, the clock is the only tick source. At equal offsets
    // MIDI is applied before the tick.
    const int* tickOffsets = nullptr;
    int numTicks = 0;

    // Transport fields for the TickClock.
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
    // (init writes NR52 = 0x80), NR50 = 0x77, NR51 = 0x00, wave RAM = slot 0; clears the note
    // stack, voices and bend; resets the resampler (primed) and DC blocker.
    void reset();

    // True for 44100, 48000, 96000 and 192000.
    static bool hostRateSupported(int sampleRate);
    // Selects the host rate (audio thread; allocates nothing; at 96/192 kHz it
    // fills RateResampler's <= 1028 coefficient table, tens of microseconds, once
    // per rate change). Returns false and changes nothing when unsupported. Takes effect fully after reset(), which
    // the caller must call next (it re-primes the resampler and the tick clock).
    bool setHostRate(int sampleRate);
    int hostRate() const { return hostRate_; }

    void applySettings(const EngineSettings& s);

    // Table / wave content the voice reads. The engine starts with an empty
    // bank of its own (no tables) and 16 default wave slots.
    rackboy::sav::RackboyBank& ownBank() { return ownBank_; }

    // Use a ContentStore (WP5) as the source of tables and waves instead of the
    // engine's own bank. The owner calls store->beginBlock() before render()
    // (it consumes staged content and refreshes the bank the engine reads). The
    // store must outlive the engine; nullptr detaches.
    void attachContent(ContentStore* store);
    ContentStore* content() const { return content_; }

    void render(const EngineBlock& block, float* left, float* right, int frames);

    // APU frames rendered since reset (the spy in tests stamps register writes
    // with this).
    uint64_t apuFrame() const { return apuFrames_; }
    rackboy_apu_t* apu() { return &apu_; }
    const MonoVoice& voice() const { return voice_; }
    const TickClock& tickClock() const { return clock_; }
    // Live table playback per channel, for the UI thread (relaxed atomics, written at the end of
    // every render() and by reset()).
    const PlaybackState& playback() const { return playback_; }
    // Audio thread: publish "nothing playing" (process() returns without rendering): the editor must
    // not keep showing the last note and playhead.
    void publishIdle();

private:
    void renderChunk(const EngineBlock& block, int frames, int c0, int n, float* left, float* right);
    void applyMidi(const EngineMidiEvent& e);
    int clockTickIn(int c0, int n) const;
    int inputsNeeded(int outFrames) const;
    long long outputCount() const;
    void resample(const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames);
    void onNoteTriggered(int blockOffset);   // restarts the tick clock's note phase
    void publishPlayback();                  // audio thread: voice + APU state -> playback_

    rackboy_apu_t apu_;
    MonoVoice voice_;
    TickClock clock_;
    ContentStore* content_ = nullptr;
    bool clockOn_ = false;     // clock_.beginBlock() was called for this render()
    enum class RateMode { Poly44, Direct48, Table };
    RateMode rateMode_ = RateMode::Poly44;
    int hostRate_ = 44100;
    int mapNum_ = 160;          // APU frames per mapDen_ host frames (48000 / 44100 reduced)
    int mapDen_ = 147;
    int dcFlushDelay_ = 240;    // host frames, see renderChunk
    long long directOut_ = 0;   // Direct48: output frames since reset
    PolyphaseResampler48To441 resampler_;
    RateResampler rateResampler_;
    DcBlocker dc_;
    EngineSettings settings_;
    rackboy::sav::RackboyBank ownBank_;
    const rackboy::sav::RackboyBank* bank_;
    uint64_t apuFrames_ = 0;
    int curHostOffset_ = 0;     // chunk-relative offset of the event being applied
    int curBlockOffset_ = 0;    // block-relative (chunk base + curHostOffset_)
    // Output frame (absolute) at which the DC blocker is flushed after a Cut,
    // or -1. See renderChunk.
    long long dcResetOut_ = -1;
    long long chunkOutBase_ = 0;
    PlaybackState playback_;
    float apuL_[kMaxApuFrames];
    float apuR_[kMaxApuFrames];
};

}  // namespace mpcboy
