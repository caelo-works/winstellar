#pragma once

#include "fits_image.h"

namespace fitsx {

// Linear-clip + Midtone-Transfer-Function parameters, in normalized [0,1]
// space relative to the data's working range.
struct StretchParams {
    float shadows = 0.0f;   // black point in [0,1]
    float highlights = 1.0f; // white point in [0,1]
    float midtone = 0.5f;   // MTF parameter; 0.5 == identity
};

// Auto-stretch using a PixInsight-style AutoSTF approach:
//   shadows clip at median - 2.8*MAD (clamped to 0)
//   highlights clip at max
//   midtone solved so that the (clipped, normalized) median maps to 0.25
StretchParams compute_auto_stretch(const FitsImage& img);

// MTF curve: f(x) = ((m-1)*x) / ((2m-1)*x - m), with f(0)=0, f(m)=0.5, f(1)=1.
// Branch-free fast path for m == 0.5 (identity).
inline float apply_mtf(float x, float m) noexcept {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    if (m == 0.5f) return x;
    const float num = (m - 1.0f) * x;
    const float den = (2.0f * m - 1.0f) * x - m;
    return num / den;
}

}  // namespace fitsx
