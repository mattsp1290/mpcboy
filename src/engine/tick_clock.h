// src/engine/tick_clock.h - table tick clock synced to the host tempo (WP6, D7).
//
// All positions are host frames (44.1 kHz) relative to the start of the current
// host block. Usage per block:
//
//   clock.beginBlock(input);
//   for each event / loop step:  int t = clock.peek(); ... clock.pop();
//   clock.noteOn(offset)        // on a trigger only, never on a glide
//   clock.endBlock();
//
// Tick 0 of a note is the note-on itself (MonoVoice applies row 0 there); the
// clock produces ticks 1, 2, ... .
//
//   Sync/Note  tick k at noteOnFrame + k * period, period = 44100*60/(bpm*tpb)
//   Free/Note  same with period = 44100 / freeHz (ignores the transport)
//   Sync/Grid  tick k at ppq = k / ticksPerBeat while `playing && ppqValid`,
//              otherwise Note behaviour continuing from the grid phase.
//
// The phase is a double distance to the next tick; integer offsets are
// floor(). No allocation, no locks.
#pragma once

#include <cstdint>

#include "channel_settings.h"

namespace mpcboy {

struct TickBlockInput
{
    int frames = 0;
    double bpm = 120.0;
    bool tempoValid = false;
    double ppqPos = 0.0;
    bool ppqValid = false;
    bool playing = false;
};

class TickClock
{
public:
    static constexpr int kMaxTicksPerBlock = 64;
    static constexpr double kSampleRate = 44100.0;

    TickClock() { reset(); }

    // Forgets the note phase, the grid state and the last valid tempo (120).
    void reset();

    void setSettings(TickMode mode, int ticksPerBeat, TickPhase phase, double freeHz);

    // Starts a block. A change of the period since the previous block rescales
    // the remaining distance to the next tick (phase fraction is kept).
    void beginBlock(const TickBlockInput& in);

    // Offset (host frames from block start, < frames) of the next tick, or -1.
    int peek() const { return nextOffset_; }
    // Consumes the tick returned by peek() and finds the next one.
    void pop();

    // Restarts the note phase at `hostFrameOffset` of the current block (call
    // only on a trigger). First tick lands one period later. In a Grid-effective
    // block the schedule stays on the grid, but the note owns the grid tick it
    // lands on: a grid tick at the note-on frame, or within half a period after
    // it, is consumed (it would otherwise apply row 1 at the same frame as row
    // 0, or an inaudibly short moment later), so row 0 is heard for about one
    // period.
    void noteOn(int hostFrameOffset);

    void endBlock();

    // Convenience for tests and offline use: beginBlock + every tick of the
    // block into `out` (capacity `cap`) + endBlock. Returns the count.
    int generate(const TickBlockInput& in, int* out, int cap);

    double bpmUsed() const { return bpmUsed_; }
    double periodFrames() const { return period_; }
    bool gridActive() const { return gridActive_; }
    // Ticks dropped because a block exceeded kMaxTicksPerBlock (debug counter).
    uint32_t droppedTicks() const { return dropped_; }

    static double periodFor(TickMode mode, double bpm, int ticksPerBeat, double freeHz);

private:
    void findNext();
    double gridDist(long long k) const;   // host frames from the block start to grid index k
    void beginGridBlock(const TickBlockInput& in);

    TickMode mode_ = TickMode::Sync;
    TickPhase phase_ = TickPhase::Note;
    int tpb_ = 24;
    double freeHz_ = kDmgFrameHz;

    double lastBpm_ = 120.0;
    double bpmUsed_ = 120.0;
    double period_ = 918.75;
    double lastPeriod_ = 0.0;

    // Note / Free phase (also continues a grid phase when the transport stops).
    bool active_ = false;
    double nextDist_ = 0.0;       // frames from the block start to the next tick

    // Grid
    bool gridActive_ = false;     // this block is grid-driven
    bool haveGridPrev_ = false;
    double predictedPpq_ = 0.0;
    long long lastK_ = -1;        // last grid index emitted (continuous playback)
    long long nextK_ = 0;
    double gridPpq0_ = 0.0;

    int frames_ = 0;
    int nextOffset_ = -1;
    int emitted_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace mpcboy
