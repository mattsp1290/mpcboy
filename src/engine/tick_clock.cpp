#include "tick_clock.h"

#include <cmath>

namespace mpcboy {

namespace {
constexpr double kGridEps = 1e-6;   // in ticks: a tick at ppq0 - eps still counts as "at ppq0"
}

double TickClock::periodFor(TickMode mode, double bpm, int ticksPerBeat, double freeHz)
{
    if (mode == TickMode::Free)
        return kSampleRate / (freeHz > 0.01 ? freeHz : 59.73);
    return kSampleRate * 60.0 / (bpm * (ticksPerBeat > 0 ? ticksPerBeat : 24));
}

void TickClock::reset()
{
    lastBpm_ = 120.0;
    bpmUsed_ = 120.0;
    period_ = periodFor(mode_, bpmUsed_, tpb_, freeHz_);
    lastPeriod_ = 0.0;
    active_ = false;
    nextDist_ = 0.0;
    gridActive_ = false;
    haveGridPrev_ = false;
    predictedPpq_ = 0.0;
    lastK_ = -1;
    nextK_ = 0;
    frames_ = 0;
    nextOffset_ = -1;
    emitted_ = 0;
}

void TickClock::setSettings(TickMode mode, int ticksPerBeat, TickPhase phase, double freeHz)
{
    if (mode != mode_ || phase != phase_) haveGridPrev_ = false;
    mode_ = mode;
    phase_ = phase;
    tpb_ = ticksPerBeat > 0 ? ticksPerBeat : 24;
    freeHz_ = freeHz > 0.01 ? freeHz : 59.73;
}

void TickClock::beginGridBlock(const TickBlockInput& in)
{
    const double ppq0 = in.ppqPos;
    const double ppqPerFrame = bpmUsed_ / (60.0 * kSampleRate);
    const double tickPpq = 1.0 / tpb_;
    const bool continuous = haveGridPrev_ && std::fabs(ppq0 - predictedPpq_) <= tickPpq;
    long long kFirst = static_cast<long long>(std::ceil(ppq0 * tpb_ - kGridEps));
    if (!continuous)
        lastK_ = kFirst - 1;          // loop / locate / tempo jump: do not emit the skipped ticks
    else if (kFirst <= lastK_)
        kFirst = lastK_ + 1;          // never tick the same grid index twice
    nextK_ = kFirst;
    gridPpq0_ = ppq0;
    framesPerTick_ = period_;
    predictedPpq_ = ppq0 + in.frames * ppqPerFrame;
    haveGridPrev_ = true;
}

void TickClock::beginBlock(const TickBlockInput& in)
{
    frames_ = in.frames;
    emitted_ = 0;
    if (in.tempoValid && in.bpm > 1.0 && in.bpm < 999.0) lastBpm_ = in.bpm;
    bpmUsed_ = lastBpm_;
    const double newPeriod = periodFor(mode_, bpmUsed_, tpb_, freeHz_);
    if (active_ && lastPeriod_ > 0.0 && newPeriod != lastPeriod_)
        nextDist_ *= newPeriod / lastPeriod_;   // keep the phase fraction
    lastPeriod_ = period_ = newPeriod;

    gridActive_ = (mode_ == TickMode::Sync && phase_ == TickPhase::Grid && in.playing && in.ppqValid);
    if (gridActive_) beginGridBlock(in);
    else haveGridPrev_ = false;
    findNext();
}

void TickClock::findNext()
{
    nextOffset_ = -1;
    if (emitted_ >= kMaxTicksPerBlock) return;
    double d;
    if (gridActive_)
    {
        const double ppqPerFrame = bpmUsed_ / (60.0 * kSampleRate);
        d = (static_cast<double>(nextK_) / tpb_ - gridPpq0_) / ppqPerFrame;
    }
    else
    {
        if (!active_) return;
        d = nextDist_;
    }
    if (d < frames_) nextOffset_ = d < 0.0 ? 0 : static_cast<int>(std::floor(d));
}

void TickClock::pop()
{
    if (nextOffset_ < 0) return;
    ++emitted_;
    if (gridActive_)
    {
        lastK_ = nextK_;
        ++nextK_;
    }
    else
    {
        nextDist_ += period_;
    }
    findNext();
}

void TickClock::noteOn(int hostFrameOffset)
{
    active_ = true;
    if (gridActive_) return;   // grid: row 1 waits for the next grid tick
    nextDist_ = static_cast<double>(hostFrameOffset) + period_;
    findNext();
}

void TickClock::endBlock()
{
    if (gridActive_)
    {
        const double ppqPerFrame = bpmUsed_ / (60.0 * kSampleRate);
        auto dist = [&](long long k) { return (static_cast<double>(k) / tpb_ - gridPpq0_) / ppqPerFrame; };
        while (dist(nextK_) < frames_) { lastK_ = nextK_; ++nextK_; ++dropped_; }   // only after the cap
        nextDist_ = dist(nextK_) - frames_;
        active_ = true;
        return;
    }
    if (!active_) return;
    while (nextDist_ < frames_) { nextDist_ += period_; ++dropped_; }
    nextDist_ -= frames_;
}

int TickClock::generate(const TickBlockInput& in, int* out, int cap)
{
    beginBlock(in);
    int n = 0;
    while (peek() >= 0)
    {
        if (n < cap) out[n++] = peek();
        pop();
    }
    endBlock();
    return n;
}

}  // namespace mpcboy
