#include "fits_core/fits_stretch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using fitsx::apply_mtf;
using fitsx::compute_auto_stretch;
using fitsx::FitsImage;
using fitsx::StretchParams;

TEST(MTF, IdentityAtHalf) {
    // m == 0.5 is the identity branch — should be exact for any x in [0,1].
    for (float x = 0.0f; x <= 1.0f; x += 0.05f) {
        EXPECT_FLOAT_EQ(apply_mtf(x, 0.5f), x);
    }
}

TEST(MTF, Endpoints) {
    // f(0)=0, f(1)=1 regardless of m (saturation guards).
    for (float m = 0.05f; m < 0.95f; m += 0.1f) {
        EXPECT_FLOAT_EQ(apply_mtf(0.0f, m), 0.0f);
        EXPECT_FLOAT_EQ(apply_mtf(1.0f, m), 1.0f);
    }
}

TEST(MTF, MidpointMapsToHalf) {
    // f(m) = 0.5 by definition of the MTF.
    for (float m = 0.1f; m < 0.9f; m += 0.05f) {
        EXPECT_NEAR(apply_mtf(m, m), 0.5f, 1e-4f);
    }
}

TEST(MTF, MonotonicallyIncreasing) {
    // For each m, f must be monotonic in x.
    for (float m : {0.10f, 0.25f, 0.50f, 0.75f, 0.90f}) {
        float prev = apply_mtf(0.0f, m);
        for (float x = 0.01f; x <= 1.0f; x += 0.01f) {
            const float cur = apply_mtf(x, m);
            EXPECT_GE(cur, prev) << "m=" << m << " x=" << x;
            prev = cur;
        }
    }
}

TEST(MTF, ClipsBelowZeroAndAboveOne) {
    // Out-of-range inputs should saturate; useful when pixel value falls
    // outside the [shadows, highlights] linear-clip range.
    EXPECT_FLOAT_EQ(apply_mtf(-0.5f, 0.3f), 0.0f);
    EXPECT_FLOAT_EQ(apply_mtf(1.5f,  0.3f), 1.0f);
}

// --- auto-stretch ----------------------------------------------------------

static FitsImage make_image(std::vector<float> data, int w, int h) {
    FitsImage img;
    img.width  = w;
    img.height = h;
    img.data   = std::move(data);
    if (!img.data.empty()) {
        float mn = img.data[0], mx = img.data[0];
        for (float v : img.data) { mn = std::min(mn, v); mx = std::max(mx, v); }
        img.source_min = mn;
        img.source_max = mx;
    }
    return img;
}

TEST(AutoStretch, ConstantImageProducesValidParams) {
    // A flat field has zero MAD — the function must still return sensible
    // (clamped) params instead of dividing by zero.
    auto img = make_image(std::vector<float>(64 * 64, 1000.0f), 64, 64);
    auto p = compute_auto_stretch(img);
    EXPECT_GE(p.shadows,    0.0f);
    EXPECT_LE(p.highlights, 1.0f);
    EXPECT_LE(p.shadows, p.highlights);
    EXPECT_TRUE(std::isfinite(p.midtone));
}

TEST(AutoStretch, BrighterFieldGivesHigherShadows) {
    // Background near 0 → shadows clip near 0.
    // Background near max → shadows clip near 1.
    auto dim = make_image(std::vector<float>(64*64, 100.0f), 64, 64);
    auto brt = make_image(std::vector<float>(64*64, 5000.0f), 64, 64);
    // Use the same source range for both so normalization is comparable.
    dim.source_min = 0.0f; dim.source_max = 10000.0f;
    brt.source_min = 0.0f; brt.source_max = 10000.0f;
    auto pd = compute_auto_stretch(dim);
    auto pb = compute_auto_stretch(brt);
    EXPECT_LT(pd.shadows, pb.shadows);
}

TEST(AutoStretch, MidtoneIsBetweenZeroAndOne) {
    std::vector<float> data(64*64);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = 500.0f + static_cast<float>(i % 257);
    }
    auto img = make_image(std::move(data), 64, 64);
    auto p = compute_auto_stretch(img);
    EXPECT_GT(p.midtone, 0.0f);
    EXPECT_LT(p.midtone, 1.0f);
}
