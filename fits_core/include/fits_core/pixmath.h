#pragma once

#include <cstddef>

#include "fits_core/fits_image.h"

// Shared per-pixel helpers used by every inspection tool (star detection, PSF
// plate, background map). The BT.601 luma weights are load-bearing -- detector,
// PSF and background must all agree on what "brightness" means, so they live in
// one place. RGB frames combine the three channels; mono reads `data` directly
// (red alone is typically the faintest astro channel and unreliable on its own).
namespace fitsx {

inline float luma_at(const FitsImage& img, std::size_t i) noexcept {
    if (!img.is_rgb()) return img.data[i];
    return 0.299f * img.data[i] + 0.587f * img.data_g[i] + 0.114f * img.data_b[i];
}

inline float luma_at(const FitsImage& img, int x, int y) noexcept {
    return luma_at(img, static_cast<std::size_t>(y) * img.width + x);
}

}  // namespace fitsx
