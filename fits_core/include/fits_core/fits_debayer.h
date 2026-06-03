#pragma once

#include "fits_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fitsx {

// Color Filter Array (Bayer) layout. The four letters of a BAYERPAT keyword
// describe the 2x2 tile in reading order: top-left, top-right, bottom-left,
// bottom-right -- as the pixels sit in the FITS-native (un-flipped) buffer.
enum class BayerPattern : uint8_t { None, RGGB, BGGR, GRBG, GBRG };

// Parse a BAYERPAT string ("RGGB", "BGGR", "GRBG", "GBRG"; case-insensitive,
// surrounding quotes/space tolerated). Returns None for anything else.
[[nodiscard]] BayerPattern parse_bayer_pattern(const std::string& s) noexcept;

// Read BAYERPAT plus the optional XBAYROFF / YBAYROFF pattern-origin offsets
// from an image's headers. Returns None (and leaves the offsets untouched at
// 0) when the image carries no recognized CFA keyword.
[[nodiscard]] BayerPattern detect_bayer_pattern(const FitsImage& img,
                                                int& xoff, int& yoff) noexcept;

// Bilinear demosaic of a single-plane CFA buffer into three full-resolution
// planes. `cfa`, `r`, `g`, `b` are all w*h, row-major, and share the SAME row
// order -- pass the FITS-native (pre-flip) buffer so the pattern letters map
// literally, then flip the three result planes downstream. `xoff`/`yoff`
// shift the pattern origin (0 unless the frame was sub-framed off a Bayer
// boundary). NaN/Inf in `cfa` must already be sanitized to 0.
void debayer_bilinear(const std::vector<float>& cfa, int w, int h,
                      BayerPattern pat, int xoff, int yoff,
                      std::vector<float>& r,
                      std::vector<float>& g,
                      std::vector<float>& b);

// Gray-world white balance: scale each channel by a per-channel gain so the
// three background medians match, neutralizing the green cast inherent to a
// raw OSC frame (2x green photosites). Operates in place. Gains are clamped
// to [0.2, 5.0] to stay sane on degenerate channels.
void gray_world_balance(std::vector<float>& r,
                        std::vector<float>& g,
                        std::vector<float>& b);

}  // namespace fitsx
