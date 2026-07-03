#pragma once

#include <cstdint>

// Sanity bounds on image geometry, enforced by every loader BEFORE any
// header-driven allocation. The loaders parse dimensions straight out of
// attacker-controllable files (FITS NAXIS*, XISF geometry="W:H:C"); a hostile
// or merely corrupt file can declare e.g. 2000000000 x 2000000000, which would
// otherwise drive a multi-terabyte std::vector allocation (bad_alloc / OOM)
// inside the in-process Explorer shell handlers. These caps reject such values
// up front. They are deliberately generous: the largest real astro sensors are
// ~150 Mpx and big mosaics a few hundred Mpx, all well under kMaxPixels.
namespace fitsx {

inline constexpr int      kMaxImageDim    = 100000;              // per-axis cap
inline constexpr uint64_t kMaxImagePixels = 1000000000ULL;       // 1 Gpx total

// True if width/height/channels are positive and within the sane caps above.
inline bool dimensions_ok(long width, long height, long channels = 1) noexcept {
    if (width <= 0 || height <= 0 || channels < 1) return false;
    if (width > kMaxImageDim || height > kMaxImageDim) return false;
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxImagePixels)
        return false;
    return true;
}

}  // namespace fitsx
