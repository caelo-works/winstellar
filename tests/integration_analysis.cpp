#include "fits_core/analysis.h"
#include "helpers/synth_fits.h"

#include <gtest/gtest.h>

#include <algorithm>

using fitsx::run_analysis;
using fitsx::FitsImage;

namespace {

FitsImage to_fitsimage(const wst::SynthFits& s) {
    FitsImage img;
    img.width  = s.width;
    img.height = s.height;
    img.data   = s.pixels;
    if (!img.data.empty()) {
        auto [mn, mx] = std::minmax_element(img.data.begin(), img.data.end());
        img.source_min = *mn;
        img.source_max = *mx;
    }
    return img;
}

}  // namespace

TEST(Analysis, ConstantImageReportsZeroStars) {
    // A flat field has no detectable stars; HFR should also be zero.
    auto spec = wst::make_constant(64, 64, 1000.0f);
    auto img  = to_fitsimage(spec);
    auto r = run_analysis(img);
    EXPECT_EQ(r.star_count, 0);
    EXPECT_DOUBLE_EQ(r.hfr_median, 0.0);
    EXPECT_DOUBLE_EQ(r.fwhm_median, 0.0);
    // Pixel stats should still be populated.
    EXPECT_NEAR(r.mean,   1000.0, 1e-3);
    EXPECT_NEAR(r.median, 1000.0, 1e-3);
    EXPECT_DOUBLE_EQ(r.stddev, 0.0);
    EXPECT_DOUBLE_EQ(r.mad,    0.0);
    EXPECT_DOUBLE_EQ(r.min_value, 1000.0);
    EXPECT_DOUBLE_EQ(r.max_value, 1000.0);
    EXPECT_EQ(r.min_count, 64u * 64u);
    EXPECT_EQ(r.max_count, 64u * 64u);
}

TEST(Analysis, StarFieldDetectsStarsAndPositiveHfr) {
    // Inject 5 Gaussian stars; the detector should find at least some of them
    // (we don't require *exactly* 5 — the algorithm has its own threshold).
    auto spec = wst::make_star_field(128, 128, /*n_stars*/ 8,
                                     /*bg*/ 1000.0f,
                                     /*bg_noise*/ 25.0f,
                                     /*peak*/ 30000.0f,
                                     /*sigma*/ 1.5f,
                                     /*seed*/ 0x1234);
    auto img  = to_fitsimage(spec);
    auto r = run_analysis(img);
    EXPECT_GT(r.star_count, 0);
    EXPECT_GT(r.hfr_median, 0.0);
    EXPECT_GT(r.fwhm_median, 0.0);
    // Sanity bounds — for sigma=1.5 the FWHM is ~3.5 px. Allow a wide window
    // (1..10 px) so the test isn't brittle if the kernel changes.
    EXPECT_GT(r.fwhm_median, 1.0);
    EXPECT_LT(r.fwhm_median, 10.0);
}

TEST(Analysis, PixelStatsMatchKnownDistribution) {
    // 0..N-1 ramp on a 32x32 image — easy median + min/max checks.
    wst::SynthFits spec;
    spec.width  = 32;
    spec.height = 32;
    spec.pixels.resize(32 * 32);
    for (size_t i = 0; i < spec.pixels.size(); ++i)
        spec.pixels[i] = static_cast<float>(i);
    auto img = to_fitsimage(spec);
    auto r = run_analysis(img);
    EXPECT_DOUBLE_EQ(r.min_value, 0.0);
    EXPECT_DOUBLE_EQ(r.max_value, 1023.0);
    EXPECT_NEAR(r.median, 511.5, 1.0);   // middle of 0..1023
}
