#pragma once

#include "fits_image.h"
#include "fits_stretch.h"

#include <cstdint>
#include <vector>

namespace fitsx {

struct RenderedBitmap {
    int width = 0;
    int height = 0;
    int stride_bytes = 0;          // = width * 4
    std::vector<uint8_t> bgra;     // size = stride_bytes * height
};

// Render the image to a 32-bit BGRA bitmap, applying linear-clip + MTF.
// If max_width or max_height is > 0, the result is downsampled (nearest-neighbor
// box average) to fit within those bounds while preserving aspect ratio.
// Pass 0 for both to render at native resolution.
RenderedBitmap render_to_bgra(const FitsImage& img,
                              const StretchParams& stretch,
                              int max_width = 0,
                              int max_height = 0);

}  // namespace fitsx
