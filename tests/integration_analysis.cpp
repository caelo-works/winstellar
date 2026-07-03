#include "fits_core/analysis.h"
#include "fits_core/psf.h"
#include "fits_core/background.h"
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

TEST(Analysis, DetailedMatchesAggregateStarCount) {
    // The detailed (per-star) path and the cached aggregate path must agree on
    // how many stars exist -- the inspection overlay would otherwise contradict
    // the measurements panel.
    auto spec = wst::make_star_field(128, 128, 8, 1000.0f, 25.0f,
                                     30000.0f, 1.5f, 0x1234);
    auto img  = to_fitsimage(spec);
    auto agg  = run_analysis(img);
    auto det  = fitsx::run_detailed_analysis(img);
    ASSERT_TRUE(det.success);
    EXPECT_EQ(static_cast<int>(det.stars.size()), agg.star_count);
    EXPECT_EQ(det.width, 128);
    EXPECT_EQ(det.height, 128);
    for (const auto& s : det.stars) {
        EXPECT_GE(s.x, 0.0f);              EXPECT_LT(s.x, 128.0f);
        EXPECT_GE(s.y, 0.0f);              EXPECT_LT(s.y, 128.0f);
        EXPECT_GT(s.hfr, 0.0f);
        EXPECT_GE(s.ecc, 0.0f);            EXPECT_LE(s.ecc, 1.0f);
    }
}

TEST(Analysis, FullAnalysisMatchesSeparateCalls) {
    // run_full_analysis must produce exactly what run_analysis + run_detailed
    // produce separately -- that equivalence is what lets the viewer compute
    // detection ONCE at load and reuse it for overlays.
    auto spec = wst::make_star_field(200, 200, 20, 1000.0f, 25.0f,
                                     30000.0f, 1.5f, 0xBEEF);
    auto img = to_fitsimage(spec);

    const auto full = fitsx::run_full_analysis(img);
    const auto agg  = run_analysis(img);
    const auto det  = fitsx::run_detailed_analysis(img);

    // Summary side.
    EXPECT_EQ(full.summary.star_count,   agg.star_count);
    EXPECT_DOUBLE_EQ(full.summary.hfr_median,  agg.hfr_median);
    EXPECT_DOUBLE_EQ(full.summary.median,      agg.median);
    EXPECT_DOUBLE_EQ(full.summary.max_value,   agg.max_value);
    // Detail side.
    EXPECT_EQ(full.detail.success, det.success);
    EXPECT_EQ(full.detail.width,   det.width);
    EXPECT_EQ(full.detail.height,  det.height);
    ASSERT_EQ(full.detail.stars.size(), det.stars.size());
    EXPECT_EQ(static_cast<int>(full.detail.stars.size()), full.summary.star_count);
    // Cross-check: same star count on both faces of the single computation.
    EXPECT_EQ(static_cast<int>(full.detail.stars.size()), agg.star_count);
}

TEST(Analysis, TiltDetectsSharpCornerOppositeSoft) {
    // Hand-built detailed result: TL corner crowded with tight stars (small
    // HFR), BR corner with bloated stars (large HFR). compute_tilt should call
    // TL the best corner, BR the worst, and report a positive tilt %.
    fitsx::DetailedAnalysis a;
    a.success = true;
    a.width = 300; a.height = 300;
    auto add = [&](float x, float y, float hfr) {
        fitsx::DetectedStar s; s.x = x; s.y = y; s.hfr = hfr; s.flux = 1.0f;
        a.stars.push_back(s);
    };
    // TL cell (0..100, 0..100): HFR ~2.0
    for (int i = 0; i < 5; ++i) add(20.0f + i, 20.0f + i, 2.0f);
    // BR cell (200..300, 200..300): HFR ~5.0
    for (int i = 0; i < 5; ++i) add(220.0f + i, 220.0f + i, 5.0f);

    auto t = fitsx::compute_tilt(a, 3);
    ASSERT_TRUE(t.success);
    EXPECT_EQ(t.grid, 3);
    EXPECT_EQ(t.best_corner, 0);   // TL
    EXPECT_EQ(t.worst_corner, 3);  // BR
    EXPECT_NEAR(t.hfr_min, 2.0, 1e-6);
    EXPECT_NEAR(t.hfr_max, 5.0, 1e-6);
    EXPECT_NEAR(t.tilt_pct, 150.0, 1e-6);   // (5-2)/2 * 100
}

TEST(Analysis, TiltEmptyOrSingleCornerHasNoMagnitude) {
    fitsx::DetailedAnalysis a;
    a.success = true; a.width = 300; a.height = 300;
    // Only one corner populated -> not enough to define tilt.
    fitsx::DetectedStar s; s.x = 20.0f; s.y = 20.0f; s.hfr = 2.0f; s.flux = 1.0f;
    a.stars.push_back(s);
    auto t = fitsx::compute_tilt(a, 3);
    EXPECT_TRUE(t.success);          // a populated cell exists
    EXPECT_DOUBLE_EQ(t.tilt_pct, 0.0);
}

TEST(Psf, MeasuresRoundStarsAsLowElongation) {
    // Big enough field that each zone has room (SR ~ 0.2*short, CUT border).
    // Round Gaussian stars -> adaptive moments should report elong ~1, and the
    // FWHM should land near 2.3548*sigma.
    auto spec = wst::make_star_field(600, 600, /*n_stars*/ 80,
                                     /*bg*/ 1000.0f, /*noise*/ 25.0f,
                                     /*peak*/ 30000.0f, /*sigma*/ 1.6f, 0xBEEF);
    auto img = to_fitsimage(spec);
    // make_star_field gives every star the same peak; the saturation cutoff
    // (max*0.92) would then flag them all. A real frame has a brightness
    // spread, so add a brighter ceiling so the 30 ke- stars sit well below it.
    img.data[0] = 65000.0f;
    auto plate = fitsx::compute_psf_plate(img, 3, 4);
    ASSERT_TRUE(plate.success);
    EXPECT_EQ(static_cast<int>(plate.zones.size()), 9);

    int total = 0; double esum = 0, fsum = 0;
    for (const auto& z : plate.zones)
        for (const auto& s : z.stars) {
            ++total; esum += s.elong; fsum += s.fwhm;
            EXPECT_GE(s.elong, 1.0f);            // a/b ratio is >= 1 by definition
            EXPECT_GT(s.snr, 15.0f);             // gate threshold
        }
    ASSERT_GT(total, 0);
    const double em = esum / total, fm = fsum / total;
    EXPECT_LT(em, 1.35);                          // round -> near-unity elongation
    EXPECT_GT(fm, 2.5); EXPECT_LT(fm, 6.0);       // ~3.77 for sigma 1.6
}

TEST(Background, FlatFieldReadsFlat) {
    auto img = to_fitsimage(wst::make_constant(900, 900, 1000.0f));
    auto bg = fitsx::compute_background(img);
    ASSERT_TRUE(bg.success);
    EXPECT_FALSE(bg.skipped);
    EXPECT_EQ(bg.rows, 48);
    EXPECT_EQ(bg.cols, 72);
    EXPECT_NEAR(bg.radial_drop, 0.0, 0.02);
    EXPECT_NEAR(bg.plane_amplitude_rel, 0.0, 0.02);
}

TEST(Background, SkipsSmallImages) {
    auto bg = fitsx::compute_background(to_fitsimage(wst::make_constant(400, 400, 1000.0f)));
    EXPECT_TRUE(bg.skipped);
    EXPECT_FALSE(bg.success);
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
