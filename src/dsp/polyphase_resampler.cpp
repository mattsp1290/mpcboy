// src/dsp/polyphase_resampler.cpp - see polyphase_resampler.h.
#include "polyphase_resampler.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mpcboy {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBeta = 10.0;                 // kHostResamplerKaiserBeta
constexpr double kFc = 44100.0 / 48000.0;      // min(1, host / apu)

// Formulas copied from rackboy_resampler.cpp so the tables agree.
double bessel_i0(double x)
{
    const double half = x * 0.5;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (half * half) / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

double sinc_pi(double x)
{
    if (x == 0.0) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

alignas(64) float g_table[PolyphaseResampler48To441::kPhases][PolyphaseResampler48To441::kTaps];
std::once_flag g_once;
std::atomic<bool> g_ready{false};

void buildTable()
{
    constexpr int N = PolyphaseResampler48To441::kTaps;
    constexpr int H = PolyphaseResampler48To441::kHalfTaps;
    constexpr int L = PolyphaseResampler48To441::kPhases;

    // Kaiser window exactly as host_resampler_init (stored as float there).
    double win[N];
    const double invI0 = 1.0 / bessel_i0(kBeta);
    for (int k = 0; k < N; ++k)
    {
        const double arg = 2.0 * static_cast<double>(k) / static_cast<double>(N - 1) - 1.0;
        const double inside = 1.0 - arg * arg;
        const double root = inside > 0.0 ? std::sqrt(inside) : 0.0;
        win[k] = static_cast<double>(static_cast<float>(bessel_i0(kBeta * root) * invI0));
    }
    for (int p = 0; p < L; ++p)
    {
        const double frac = static_cast<double>(p) / static_cast<double>(L);
        for (int k = 0; k < N; ++k)
        {
            const double tau = frac - static_cast<double>(k - H);
            g_table[p][k] = static_cast<float>(kFc * sinc_pi(kFc * tau) * win[k]);
        }
    }
    g_ready.store(true, std::memory_order_release);
}

// 257-tap dot product, 8 independent float accumulators (vectorises without
// fast-math), tap 256 added last.
inline float dot257(const float* __restrict x, const float* __restrict c)
{
    float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 256; k += 8)
        for (int j = 0; j < 8; ++j)
            acc[j] += x[k + j] * c[k + j];
    float s = ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
    return s + x[256] * c[256];
}

inline float flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }

}  // namespace

void PolyphaseResampler48To441::ensureTable()
{
    std::call_once(g_once, buildTable);
}

const float* PolyphaseResampler48To441::phaseRow(int phase)
{
    ensureTable();
    return g_table[phase];
}

PolyphaseResampler48To441::PolyphaseResampler48To441()
{
    ensureTable();
    reset(0);
}

void PolyphaseResampler48To441::reset(int zeroPrefill)
{
    std::memset(work_, 0, sizeof(work_));
    inputCount_ = zeroPrefill < 0 ? 0 : zeroPrefill;
    outputCount_ = 0;
}

int PolyphaseResampler48To441::inputsNeeded(int outFrames) const
{
    if (outFrames <= 0) return 0;
    const long long lastM = outputCount_ + outFrames - 1;
    const long long latest = (lastM * kStep) / kPhases + kHalfTaps;
    const long long need = latest + 1 - inputCount_;
    return need > 0 ? static_cast<int>(need) : 0;
}

void PolyphaseResampler48To441::process(const float* inL, const float* inR, int inFrames,
                                        float* outL, float* outR, int outFrames)
{
    if (inFrames < 0) inFrames = 0;
    if (inFrames > kMaxInFrames) inFrames = kMaxInFrames;
    if (outFrames < 0) outFrames = 0;

    // work_[c] = [ kHistory previous input frames | this call's chunk ].
    // Absolute index of work_[c][0] is inputCount_ - kHistory (zeros before 0).
    const float* in[2] = {inL, inR};
    for (int c = 0; c < 2; ++c)
        if (inFrames > 0) std::memcpy(work_[c] + kHistory, in[c], sizeof(float) * static_cast<size_t>(inFrames));

    const long long wBase = inputCount_ - kHistory;
    const int avail = kHistory + inFrames;
    float* out[2] = {outL, outR};

    long long num = outputCount_ * kStep;
    long long base = num / kPhases;
    int ph = static_cast<int>(num - base * kPhases);

    for (int i = 0; i < outFrames; ++i)
    {
        const long long startLL = base - kHalfTaps - wBase;
        if (startLL >= 0 && startLL + kTaps <= avail)
        {
            const float* row = g_table[ph];
            const int start = static_cast<int>(startLL);
            out[0][i] = flush(dot257(work_[0] + start, row));
            out[1][i] = flush(dot257(work_[1] + start, row));
        }
        else
        {
            out[0][i] = 0.0f;   // not enough input pushed for this output
            out[1][i] = 0.0f;
        }
        // advance output index by one: input time grows by 160/147
        base += kStep / kPhases;                     // 1
        ph += kStep % kPhases;                       // 13
        if (ph >= kPhases) { ph -= kPhases; ++base; }
    }

    // Keep the newest kHistory frames at the head for the next call.
    for (int c = 0; c < 2; ++c)
        std::memmove(work_[c], work_[c] + inFrames, sizeof(float) * kHistory);

    inputCount_ += inFrames;
    outputCount_ += outFrames;
}

}  // namespace mpcboy
