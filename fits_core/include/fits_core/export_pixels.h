#pragma once

#include "fits_image.h"
#include "fits_render.h"
#include "image_rotate.h"

#include <cstddef>
#include <cstdint>

namespace fitsx {

// Pixel arithmetic for the image-export lane. It lives in fits_core, not in the
// viewer, so it can be tested without COM or a window: the WIC code above it
// then has no arithmetic left to get wrong.

// How linear float data maps onto 16-bit integers.
struct Linear16Scale {
    // Every sample is already an integer in [0, 65535], so the export writes
    // them through unchanged. This is the common case -- a BITPIX=16 FITS with
    // BZERO=32768, a UInt16 XISF, a 16-bit RAW -- and it is what makes "the
    // same numbers in another tool" literally true.
    bool   identity = false;
    double lo = 0.0, hi = 1.0;   // affine anchors, used only when !identity
    // Set when the data was constant and `hi` had to be nudged to keep the
    // mapping well defined. Callers may want to mention it.
    bool   degenerate_anchors_replaced = false;
};

// One pass over every plane. Establishes the TRUE data range and whether the
// samples are already 16-bit integers.
//
// FitsImage::source_min/source_max are deliberately NOT used as anchors: the
// loader substitutes [0,1] whenever the range is degenerate, which is a lie
// about a constant-valued frame and would silently rescale real data.
[[nodiscard]] Linear16Scale compute_linear16_scale(const FitsImage& img);

// Row-block packers. `y0` is the first OUTPUT row and `rows` the count, so a
// caller can stream a large frame through a small staging buffer instead of
// materialising a second full copy. `dst` must hold rows * m.out_w * bpp bytes.
//
// All three apply `m`, so rotation happens exactly once, in one place.

// 32bpp BGRA -> 24bpp BGR: drops the alpha byte. JPEG has no alpha channel and
// PNG would otherwise carry a meaningless one.
void pack_bgr24_rows(const RenderedBitmap& src, const RotMap& m,
                     int y0, int rows, std::uint8_t* dst) noexcept;

// Linear mono plane -> 16bpp gray.
void pack_gray16_rows(const FitsImage& img, const RotMap& m, const Linear16Scale& s,
                      int y0, int rows, std::uint16_t* dst) noexcept;

// Linear RGB planes -> 48bpp RGB, channel order R, G, B.
void pack_rgb48_rows(const FitsImage& img, const RotMap& m, const Linear16Scale& s,
                     int y0, int rows, std::uint16_t* dst) noexcept;

// Element-wise rotation for the FITS lane, which moves samples at their native
// width (1, 2, 4 or 8 bytes) without ever converting them.
void rotate_elements(const void* src, const RotMap& m, std::size_t elem_bytes,
                     void* dst) noexcept;

}  // namespace fitsx
