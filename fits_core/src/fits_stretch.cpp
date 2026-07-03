#include "fits_core/fits_stretch.h"
#include "fits_core/stats.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fitsx {

namespace {

// median_inplace lives in fits_core/stats.h.

}  // namespace

StretchParams compute_auto_stretch(const FitsImage& img) {
    StretchParams p;
    if (img.empty()) return p;

    const double range = img.source_max - img.source_min;
    if (range <= 0.0) return p;

    // Sample the data: cap at ~100k points for stable, fast statistics.
    const size_t total = img.pixel_count();
    constexpr size_t kTargetSamples = 100000;
    const size_t step = std::max<size_t>(1, total / kTargetSamples);

    std::vector<float> samples;
    samples.reserve(total / step + 1);
    const float vmin = static_cast<float>(img.source_min);
    const float frange = static_cast<float>(range);
    for (size_t i = 0; i < total; i += step) {
        const float v = img.data[i];
        if (std::isfinite(v)) {
            samples.push_back((v - vmin) / frange);
        }
    }
    if (samples.empty()) return p;

    std::vector<float> work = samples;
    const float median = median_inplace(work);

    std::vector<float> dev;
    dev.reserve(samples.size());
    for (float v : samples) dev.push_back(std::fabs(v - median));
    const float mad = median_inplace(dev);

    // PixInsight AutoSTF defaults: target background 0.25, shadows clip at -2.8 sigma.
    constexpr float kTargetBg = 0.25f;
    constexpr float kShadowsClip = -2.8f;
    constexpr float kMadToSigma = 1.4826f;

    float shadows = std::clamp(median + kShadowsClip * kMadToSigma * mad, 0.0f, 1.0f);
    float highlights = 1.0f;

    const float denom = highlights - shadows;
    if (denom <= 0.0f) return p;
    const float xn = std::clamp((median - shadows) / denom, 0.0f, 1.0f);

    // Solve MTF(midtone, xn) = kTargetBg :
    //   midtone = xn * (1 - B) / (xn + B - 2 * xn * B)
    float midtone = 0.5f;
    const float num = xn * (1.0f - kTargetBg);
    const float den = xn + kTargetBg - 2.0f * xn * kTargetBg;
    if (den > 0.0f && num > 0.0f) {
        midtone = std::clamp(num / den, 0.001f, 0.999f);
    }

    p.shadows = shadows;
    p.highlights = highlights;
    p.midtone = midtone;
    return p;
}

}  // namespace fitsx
