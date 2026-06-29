#pragma once

#include <algorithm>

#include <d2d1.h>

// HFR-quality colour ramp shared by the inspection overlays: green (tight) ->
// amber -> red (bloated). t in [0,1]. Used by the on-image star markers and the
// tilt diagram so the two never drift apart.
inline D2D1::ColorF quality_color(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    // 0 -> green (0.30,0.85,0.40), 0.5 -> amber (0.98,0.78,0.20),
    // 1 -> red (0.95,0.30,0.25). Two-segment linear blend.
    float r, g, b;
    if (t < 0.5f) {
        const float u = t / 0.5f;
        r = 0.30f + u * (0.98f - 0.30f);
        g = 0.85f + u * (0.78f - 0.85f);
        b = 0.40f + u * (0.20f - 0.40f);
    } else {
        const float u = (t - 0.5f) / 0.5f;
        r = 0.98f + u * (0.95f - 0.98f);
        g = 0.78f + u * (0.30f - 0.78f);
        b = 0.20f + u * (0.25f - 0.20f);
    }
    return D2D1::ColorF(r, g, b);
}
