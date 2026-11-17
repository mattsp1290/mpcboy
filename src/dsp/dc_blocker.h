// src/dsp/dc_blocker.h
//
// Stereo one-pole DC blocker, y[n] = x[n] - x[n-1] + a * y[n-1], with the same
// coefficient, NaN guard and denormal flush as taps_process_channel in
// src/core/rackboy_taps.cpp (alpha = kDcBlockerAlpha = 0.995 at 44.1 kHz).
// The corner frequency is held constant in Hz across host rates: setSampleRate
// derives fc from 0.995 at 44.1 kHz (fc = -ln(0.995) * 44100 / 2 pi, about 35 Hz)
// and uses alpha = exp(-2 pi fc / sr) elsewhere. 44100 uses the literal 0.995f
// (bit-identical to the MPC path); 48 kHz gives about 0.99543.
#pragma once

#include <cmath>

#include "rackboy_taps.h"

namespace mpcboy {

struct DcBlocker
{
    float xl = 0.0f, yl = 0.0f, xr = 0.0f, yr = 0.0f;
    float a = rackboy::core::kDcBlockerAlpha;

    // Not on the hot path; no allocation. Keeps the filter state.
    void setSampleRate(int sr)
    {
        if (sr == 44100) { a = rackboy::core::kDcBlockerAlpha; return; }
        constexpr double kTwoPi = 6.283185307179586;
        const double fc = -std::log(static_cast<double>(rackboy::core::kDcBlockerAlpha)) * 44100.0 / kTwoPi;
        a = static_cast<float>(std::exp(-kTwoPi * fc / static_cast<double>(sr)));
    }

    void reset() { xl = yl = xr = yr = 0.0f; }

    // In place, n frames.
    void process(float* l, float* r, int n)
    {
        const float a = this->a;
        constexpr float tiny = rackboy::core::kDenormalThreshold;
        float pxl = xl, pyl = yl, pxr = xr, pyr = yr;
        for (int i = 0; i < n; ++i)
        {
            float x = l[i];
            if (std::isnan(x)) x = 0.0f;
            float y = x - pxl + a * pyl;
            if (std::isnan(y)) y = 0.0f;
            if (std::fabs(y) < tiny) y = 0.0f;
            l[i] = y; pxl = x; pyl = y;

            x = r[i];
            if (std::isnan(x)) x = 0.0f;
            y = x - pxr + a * pyr;
            if (std::isnan(y)) y = 0.0f;
            if (std::fabs(y) < tiny) y = 0.0f;
            r[i] = y; pxr = x; pyr = y;
        }
        xl = pxl; yl = pyl; xr = pxr; yr = pyr;
    }
};

}  // namespace mpcboy
