// src/dsp/dc_blocker.h
//
// Stereo one-pole DC blocker, y[n] = x[n] - x[n-1] + a * y[n-1], with the same
// coefficient, NaN guard and denormal flush as taps_process_channel in
// src/core/rackboy_taps.cpp (alpha = kDcBlockerAlpha = 0.995).
#pragma once

#include <cmath>

#include "rackboy_taps.h"

namespace mpcboy {

struct DcBlocker
{
    float xl = 0.0f, yl = 0.0f, xr = 0.0f, yr = 0.0f;

    void reset() { xl = yl = xr = yr = 0.0f; }

    // In place, n frames.
    void process(float* l, float* r, int n)
    {
        constexpr float a = rackboy::core::kDcBlockerAlpha;
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
