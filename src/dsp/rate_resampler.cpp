// src/dsp/rate_resampler.cpp - see rate_resampler.h.
#include "rate_resampler.h"

#include <cmath>
#include <cstring>
#include <numeric>

namespace mpcboy {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBeta = 10.0;   // kHostResamplerKaiserBeta

// Formulas copied from rackboy_resampler.cpp (as in polyphase_resampler.cpp) so the tables agree.
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

// Same dot product as polyphase_resampler.cpp: 8 float accumulators, tap 256 last.
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

RateResampler::RateResampler()
{
    // Kaiser window exactly as host_resampler_init (stored as float there).
    const double invI0 = 1.0 / bessel_i0(kBeta);
    for (int k = 0; k < kTaps; ++k)
    {
        const double arg = 2.0 * static_cast<double>(k) / static_cast<double>(kTaps - 1) - 1.0;
        const double inside = 1.0 - arg * arg;
        const double root = inside > 0.0 ? std::sqrt(inside) : 0.0;
        win_[k] = static_cast<float>(bessel_i0(kBeta * root) * invI0);
    }
    std::memset(table_, 0, sizeof(table_));
    reset(0);
}

bool RateResampler::rateSupported(int hostRate)
{
    if (hostRate < kApuRate) return false;
    const int g = std::gcd(kApuRate, hostRate);
    return hostRate / g <= kMaxPhases;
}

bool RateResampler::setRate(int hostRate)
{
    if (!rateSupported(hostRate)) return false;
    const int g = std::gcd(kApuRate, hostRate);
    hostRate_ = hostRate;
    phases_ = hostRate / g;
    step_ = kApuRate / g;
    const double fc = 1.0;   // min(1, host / apu) for host >= apu
    for (int p = 0; p < phases_; ++p)
    {
        const double frac = static_cast<double>(p) / static_cast<double>(phases_);
        for (int k = 0; k < kTaps; ++k)
        {
            const double tau = frac - static_cast<double>(k - kHalfTaps);
            table_[p][k] = static_cast<float>(fc * sinc_pi(fc * tau) * static_cast<double>(win_[k]));
        }
    }
    reset(0);
    return true;
}

void RateResampler::reset(int zeroPrefill)
{
    std::memset(work_, 0, sizeof(work_));
    inputCount_ = zeroPrefill < 0 ? 0 : zeroPrefill;
    outputCount_ = 0;
}

int RateResampler::inputsNeeded(int outFrames) const
{
    if (outFrames <= 0 || phases_ == 0) return 0;
    const long long lastM = outputCount_ + outFrames - 1;
    const long long latest = (lastM * step_) / phases_ + kHalfTaps;
    const long long need = latest + 1 - inputCount_;
    return need > 0 ? static_cast<int>(need) : 0;
}

void RateResampler::process(const float* inL, const float* inR, int inFrames,
                            float* outL, float* outR, int outFrames)
{
    if (inFrames < 0) inFrames = 0;
    if (inFrames > kMaxInFrames) inFrames = kMaxInFrames;
    if (outFrames < 0) outFrames = 0;
    if (phases_ == 0)
    {
        for (int i = 0; i < outFrames; ++i) outL[i] = outR[i] = 0.0f;
        return;
    }

    // work_[c] = [ kHistory previous input frames | this call's chunk ].
    const float* in[2] = {inL, inR};
    for (int c = 0; c < 2; ++c)
        if (inFrames > 0) std::memcpy(work_[c] + kHistory, in[c], sizeof(float) * static_cast<size_t>(inFrames));

    const long long wBase = inputCount_ - kHistory;
    const int avail = kHistory + inFrames;
    float* out[2] = {outL, outR};

    const long long num = outputCount_ * step_;
    long long base = num / phases_;
    int ph = static_cast<int>(num - base * phases_);

    for (int i = 0; i < outFrames; ++i)
    {
        const long long startLL = base - kHalfTaps - wBase;
        if (startLL >= 0 && startLL + kTaps <= avail)
        {
            const float* row = table_[ph];
            const int start = static_cast<int>(startLL);
            out[0][i] = flush(dot257(work_[0] + start, row));
            out[1][i] = flush(dot257(work_[1] + start, row));
        }
        else
        {
            out[0][i] = 0.0f;   // not enough input pushed for this output
            out[1][i] = 0.0f;
        }
        base += step_ / phases_;
        ph += step_ % phases_;
        if (ph >= phases_) { ph -= phases_; ++base; }
    }

    for (int c = 0; c < 2; ++c)
        std::memmove(work_[c], work_[c] + inFrames, sizeof(float) * kHistory);

    inputCount_ += inFrames;
    outputCount_ += outFrames;
}

}  // namespace mpcboy
