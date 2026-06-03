#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wst {

// Description of a synthetic 32-bit-float FITS file generated for tests.
// `pixels` is row-major in FITS order (row 0 = bottom of image).
struct SynthFits {
    int width  = 32;
    int height = 32;
    std::vector<float> pixels;       // size = width * height
    std::vector<std::pair<std::string, std::string>> extra_keywords;
};

// Write a minimal valid FITS file to `path` (UTF-8). Returns the absolute path
// written, or empty on failure. Caller is responsible for deleting the file
// (we use a per-test temp directory in the test fixtures themselves).
std::string write_synth_fits(const std::string& path, const SynthFits& spec);

// Build a deterministic image with a few injected Gaussian "stars" on top of
// a noisy background. The analyzer's star detector needs MAD > 0 to compute
// a sigma threshold, so the noise floor must be non-zero or detection
// trivially fails on a perfectly flat field.
SynthFits make_star_field(int width = 128, int height = 128,
                          int n_stars = 5,
                          float background = 1000.0f,
                          float bg_noise_sigma = 25.0f,
                          float star_peak = 30000.0f,
                          float star_sigma = 1.5f,
                          unsigned seed = 0xC0FFEE);

// Constant-value image; used for stretch/auto-stretch fixtures.
SynthFits make_constant(int width, int height, float value);

// Write a 3-plane (NAXIS=3, NAXIS3=3) RGB cube FITS, planar in FITS order
// (full R plane, then G, then B). Each plane is width*height, row 0 = bottom.
// Returns the path written, or empty on failure.
std::string write_synth_fits_rgb_cube(const std::string& path,
                                      int width, int height,
                                      const std::vector<float>& r,
                                      const std::vector<float>& g,
                                      const std::vector<float>& b);

}  // namespace wst
