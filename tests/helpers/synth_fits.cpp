#include "synth_fits.h"

#include <fitsio.h>

#include <cmath>
#include <cstring>
#include <random>

namespace wst {

std::string write_synth_fits(const std::string& path, const SynthFits& spec) {
    if (spec.width <= 0 || spec.height <= 0) return {};
    if (static_cast<size_t>(spec.width) * spec.height != spec.pixels.size()) return {};

    fitsfile* fp = nullptr;
    int status = 0;
    // CFITSIO refuses to overwrite by default; use the "!" prefix.
    const std::string create_path = "!" + path;
    if (fits_create_file(&fp, create_path.c_str(), &status) != 0) return {};

    long naxes[2] = { spec.width, spec.height };
    if (fits_create_img(fp, FLOAT_IMG, 2, naxes, &status) != 0) {
        fits_close_file(fp, &status);
        return {};
    }

    long fpixel[2] = { 1, 1 };
    if (fits_write_pix(fp, TFLOAT, fpixel,
                       static_cast<long>(spec.pixels.size()),
                       const_cast<float*>(spec.pixels.data()), &status) != 0) {
        fits_close_file(fp, &status);
        return {};
    }

    // Inject extra keywords (string values; comments empty).
    for (const auto& kv : spec.extra_keywords) {
        // fits_update_key_str doesn't take const char*; cast away.
        fits_update_key(fp, TSTRING,
                        const_cast<char*>(kv.first.c_str()),
                        const_cast<char*>(kv.second.c_str()),
                        nullptr, &status);
    }

    fits_close_file(fp, &status);
    return (status == 0) ? path : std::string{};
}

std::string write_synth_fits_rgb_cube(const std::string& path,
                                      int width, int height,
                                      const std::vector<float>& r,
                                      const std::vector<float>& g,
                                      const std::vector<float>& b) {
    if (width <= 0 || height <= 0) return {};
    const size_t npix = static_cast<size_t>(width) * height;
    if (r.size() != npix || g.size() != npix || b.size() != npix) return {};

    fitsfile* fp = nullptr;
    int status = 0;
    const std::string create_path = "!" + path;
    if (fits_create_file(&fp, create_path.c_str(), &status) != 0) return {};

    long naxes[3] = { width, height, 3 };
    if (fits_create_img(fp, FLOAT_IMG, 3, naxes, &status) != 0) {
        fits_close_file(fp, &status);
        return {};
    }

    // Planar write: R plane fills pixels [1..npix], G the next, B the last.
    std::vector<float> planar;
    planar.reserve(npix * 3);
    planar.insert(planar.end(), r.begin(), r.end());
    planar.insert(planar.end(), g.begin(), g.end());
    planar.insert(planar.end(), b.begin(), b.end());

    long fpixel[3] = { 1, 1, 1 };
    if (fits_write_pix(fp, TFLOAT, fpixel,
                       static_cast<long>(planar.size()),
                       planar.data(), &status) != 0) {
        fits_close_file(fp, &status);
        return {};
    }

    fits_close_file(fp, &status);
    return (status == 0) ? path : std::string{};
}

SynthFits make_constant(int width, int height, float value) {
    SynthFits s;
    s.width = width;
    s.height = height;
    s.pixels.assign(static_cast<size_t>(width) * height, value);
    return s;
}

SynthFits make_star_field(int width, int height, int n_stars,
                          float background, float bg_noise_sigma,
                          float star_peak, float star_sigma, unsigned seed) {
    SynthFits s;
    s.width  = width;
    s.height = height;
    s.pixels.resize(static_cast<size_t>(width) * height);

    std::mt19937 rng(seed);
    // Gaussian background noise — without it the analyzer's MAD is zero and
    // the threshold collapses to the median, so no star ever rises above.
    std::normal_distribution<float> noise(background, bg_noise_sigma);
    for (auto& v : s.pixels) v = noise(rng);
    // Keep stars away from the border so the analysis kernel can fit a fit.
    const int margin = static_cast<int>(std::ceil(3.0f * star_sigma)) + 2;
    std::uniform_int_distribution<int> rx(margin, width  - margin - 1);
    std::uniform_int_distribution<int> ry(margin, height - margin - 1);

    const float two_sigma2 = 2.0f * star_sigma * star_sigma;
    const int   kradius    = static_cast<int>(std::ceil(3.0f * star_sigma));

    for (int i = 0; i < n_stars; ++i) {
        const int cx = rx(rng);
        const int cy = ry(rng);
        for (int dy = -kradius; dy <= kradius; ++dy) {
            for (int dx = -kradius; dx <= kradius; ++dx) {
                const float r2  = static_cast<float>(dx*dx + dy*dy);
                const float val = star_peak * std::exp(-r2 / two_sigma2);
                const int   px  = cx + dx;
                const int   py  = cy + dy;
                if (px < 0 || px >= width || py < 0 || py >= height) continue;
                s.pixels[static_cast<size_t>(py) * width + px] += val;
            }
        }
    }
    return s;
}

}  // namespace wst
