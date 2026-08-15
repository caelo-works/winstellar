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

// Bayer pattern of the array once the DISPLAY has been rotated `display_rotation_deg`
// clockwise. `xoff`/`yoff` are the source XBAYROFF/YBAYROFF; they are folded into
// the result, so a caller writes the returned pattern with XBAYROFF = YBAYROFF = 0.
// `w`/`h` are the SOURCE array dimensions: odd values shift the tile phase, which
// is why this is evaluated rather than looked up in a table.
//
// This matters because rotating a CFA mosaic changes the effective pattern. Writing
// the rotated mosaic while keeping the original BAYERPAT produces a file that opens
// fine and demosaics with swapped colours -- silent, and invisible in the file
// itself. Returns None only when `src` is None.
[[nodiscard]] BayerPattern rotate_bayer_pattern(BayerPattern src, int xoff, int yoff,
                                                int w, int h,
                                                int display_rotation_deg) noexcept;

// "RGGB" / "BGGR" / "GRBG" / "GBRG", or "" for None. Never null.
[[nodiscard]] const char* bayer_pattern_name(BayerPattern p) noexcept;

// Gray-world white balance: scale each channel by a per-channel gain so the
// three background medians match, neutralizing the green cast inherent to a
// raw OSC frame (2x green photosites). Operates in place. Gains are clamped
// to [0.2, 5.0] to stay sane on degenerate channels.
void gray_world_balance(std::vector<float>& r,
                        std::vector<float>& g,
                        std::vector<float>& b);

}  // namespace fitsx
