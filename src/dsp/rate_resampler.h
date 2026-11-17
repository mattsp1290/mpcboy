// src/dsp/rate_resampler.h
//
// 48 kHz -> host-rate stereo polyphase resampler for desktop hosts at 96 and
// 192 kHz (and any rate >= 48 kHz whose reduced ratio has at most kMaxPhases
// phases). mpcboy-owned sibling of PolyphaseResampler48To441.
//
// Same kernel and time axis as the vendored rackboy::resampler::HostResampler
// (257-tap Kaiser-windowed sinc, beta = 10, fc = min(1, host / 48000), output m
// centred on input time m * M / L), but the L distinct fractional phases are
// tabulated when the rate is set instead of calling std::sin 257 times per
// output frame. 48 kHz -> 96 kHz has L = 2 phases, -> 192 kHz has L = 4.
//
// Group delay: the kernel half width is kHalfTaps = 128 APU frames, i.e.
// 128 * hostRate / 48000 host frames (2.67 ms at every rate): 256 host frames at
// 96 kHz, 512 at 192 kHz (the engine primes 128 zero APU frames, exactly that
// delay).
//
// Threading: the Kaiser window is computed in the constructor. setRate() fills
// the table (at most kMaxPhases * kTaps = 1028 sinc evaluations, tens of
// microseconds); it allocates nothing, so it is safe on the audio thread, but it
// is not free: call it only at a rate change. process() allocates nothing.
#pragma once

namespace mpcboy {

class RateResampler
{
public:
    static constexpr int kMaxPhases = 4;
    static constexpr int kTaps = 257;
    static constexpr int kHalfTaps = (kTaps - 1) / 2;   // 128
    static constexpr int kHistory = 320;                 // >= kTaps - 1, as the 44.1 kHz class
    static constexpr int kMaxInFrames = 1024;
    static constexpr int kApuRate = 48000;

    RateResampler();

    // True when hostRate >= 48000 and its reduced ratio to 48 kHz has <= kMaxPhases phases.
    static bool rateSupported(int hostRate);

    // Builds the coefficient table for hostRate and resets the counters. Returns
    // false (nothing changed) for an unsupported rate. No allocation.
    bool setRate(int hostRate);

    // Clear history and counters; `zeroPrefill` pretends that many zero input frames were pushed.
    void reset(int zeroPrefill = 0);

    // Input frames that must be passed to process() to emit `outFrames`.
    int inputsNeeded(int outFrames) const;

    // Push `inFrames` (<= kMaxInFrames) input frames, emit `outFrames`. setRate() must have succeeded.
    void process(const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames);

    long long inputCount() const { return inputCount_; }
    long long outputCount() const { return outputCount_; }
    int phases() const { return phases_; }
    int hostRate() const { return hostRate_; }

private:
    float win_[kTaps];
    alignas(64) float table_[kMaxPhases][kTaps];
    float work_[2][kHistory + kMaxInFrames];
    int hostRate_ = 0;
    int phases_ = 0;    // L
    int step_ = 0;      // M
    long long inputCount_ = 0;
    long long outputCount_ = 0;
};

}  // namespace mpcboy
