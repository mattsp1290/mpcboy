// src/dsp/polyphase_resampler.h
//
// 48 kHz -> 44.1 kHz stereo polyphase resampler (WP3, decision D5).
//
// Same kernel as the vendored reference rackboy::resampler::HostResampler
// (257-tap Kaiser-windowed sinc, beta = 10, fc = 44100/48000), but with the
// 147 distinct fractional phases (48000/44100 = 160/147) tabulated once
// instead of calling std::sin 257 times per output sample.
//
// Time axis (identical to HostResampler): output m is centred on input time
// m * 160 / 147, so it needs inputs up to index floor(m * 160 / 147) + 128.
// Inputs before index 0 are zero. The caller therefore feeds
// inputsNeeded(outFrames) frames per call and gets exactly outFrames back.
//
// Threading: the coefficient table is process-wide and built under
// std::call_once from the constructor / ensureTable() (never on the audio
// thread once an instance exists). process() allocates nothing.
#pragma once

#include <cstdint>

namespace mpcboy {

class PolyphaseResampler48To441
{
public:
    static constexpr int kPhases = 147;      // 44100 / gcd
    static constexpr int kStep = 160;        // 48000 / gcd: input advance per kPhases outputs
    static constexpr int kTaps = 257;
    static constexpr int kHalfTaps = (kTaps - 1) / 2;   // 128
    // Input history kept between calls. The window of one output spans kTaps
    // samples, so at least kTaps - 1 = 256 older samples are needed (the plan
    // says 256; the extra margin covers the output whose window starts one
    // sample before the carried history).
    static constexpr int kHistory = 320;
    // Largest input chunk per process() call.
    static constexpr int kMaxInFrames = 1024;

    PolyphaseResampler48To441();

    // Builds the shared coefficient table (idempotent, thread-safe). Call from
    // a constructor / prepare, never from the audio thread.
    static void ensureTable();

    // Coefficient row for a phase, kTaps floats (exposed for tests).
    static const float* phaseRow(int phase);

    // Clear history and counters. `zeroPrefill` pretends that many zero input
    // frames were already pushed (the engine primes 128 so the first host
    // block does not need an oversized APU render). 0 reproduces
    // HostResampler's time axis exactly.
    void reset(int zeroPrefill = 0);

    // Input frames that must be passed to process() to emit `outFrames`.
    int inputsNeeded(int outFrames) const;

    // Push `inFrames` (<= kMaxInFrames) stereo input frames and emit
    // `outFrames` stereo output frames. inFrames should equal
    // inputsNeeded(outFrames); outputs whose window is not fully covered by
    // the frames pushed so far are emitted as zero.
    void process(const float* inL, const float* inR, int inFrames,
                 float* outL, float* outR, int outFrames);

    long long inputCount() const { return inputCount_; }
    long long outputCount() const { return outputCount_; }

private:
    float work_[2][kHistory + kMaxInFrames];
    long long inputCount_ = 0;    // absolute input frames consumed (incl. prefill)
    long long outputCount_ = 0;   // absolute output frames emitted
};

}  // namespace mpcboy
