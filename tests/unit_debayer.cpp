#include "fits_core/fits_debayer.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using fitsx::BayerPattern;

namespace {

// Color codes mirror fits_debayer.cpp internals: 0=R, 1=G, 2=B.
int color_rggb(int x, int y) {
    static const int t[2][2] = { {0, 1}, {1, 2} };  // RGGB tile
    return t[y & 1][x & 1];
}

}  // namespace

// --------------------------------------------------------------------------
//  Pattern parsing
// --------------------------------------------------------------------------

TEST(Debayer, ParsesAllPatterns) {
    EXPECT_EQ(fitsx::parse_bayer_pattern("RGGB"), BayerPattern::RGGB);
    EXPECT_EQ(fitsx::parse_bayer_pattern("BGGR"), BayerPattern::BGGR);
    EXPECT_EQ(fitsx::parse_bayer_pattern("GRBG"), BayerPattern::GRBG);
    EXPECT_EQ(fitsx::parse_bayer_pattern("GBRG"), BayerPattern::GBRG);
}

TEST(Debayer, ParseToleratesQuotesAndCase) {
    EXPECT_EQ(fitsx::parse_bayer_pattern("'rggb'"), BayerPattern::RGGB);
    EXPECT_EQ(fitsx::parse_bayer_pattern("  RgGb "), BayerPattern::RGGB);
}

TEST(Debayer, ParseRejectsJunk) {
    EXPECT_EQ(fitsx::parse_bayer_pattern(""),      BayerPattern::None);
    EXPECT_EQ(fitsx::parse_bayer_pattern("XYZW"),  BayerPattern::None);
    EXPECT_EQ(fitsx::parse_bayer_pattern("RGB"),   BayerPattern::None);
}

// --------------------------------------------------------------------------
//  Pattern parity: a single bright pixel must land in the channel the tile
//  assigns to (0,0). This is the test that catches a swapped R/B or a wrong
//  tile mapping -- the failure the row-flip ordering is designed to avoid.
// --------------------------------------------------------------------------

TEST(Debayer, SinglePixelLandsInCorrectChannel) {
    const int w = 6, h = 6;
    auto run = [&](BayerPattern p, int xoff, int yoff) {
        std::vector<float> cfa(static_cast<size_t>(w) * h, 0.0f);
        cfa[0] = 1000.0f;  // pixel (0,0)
        std::vector<float> r, g, b;
        fitsx::debayer_bilinear(cfa, w, h, p, xoff, yoff, r, g, b);
        return std::array<float, 3>{ r[0], g[0], b[0] };
    };

    // RGGB: (0,0) is Red.
    { auto c = run(BayerPattern::RGGB, 0, 0);
      EXPECT_FLOAT_EQ(c[0], 1000.0f); EXPECT_FLOAT_EQ(c[1], 0.0f); EXPECT_FLOAT_EQ(c[2], 0.0f); }
    // BGGR: (0,0) is Blue.
    { auto c = run(BayerPattern::BGGR, 0, 0);
      EXPECT_FLOAT_EQ(c[2], 1000.0f); EXPECT_FLOAT_EQ(c[0], 0.0f); EXPECT_FLOAT_EQ(c[1], 0.0f); }
    // GRBG / GBRG: (0,0) is Green.
    { auto c = run(BayerPattern::GRBG, 0, 0); EXPECT_FLOAT_EQ(c[1], 1000.0f); }
    { auto c = run(BayerPattern::GBRG, 0, 0); EXPECT_FLOAT_EQ(c[1], 1000.0f); }

    // An x-offset of 1 shifts RGGB's (0,0) from R to G.
    { auto c = run(BayerPattern::RGGB, 1, 0); EXPECT_FLOAT_EQ(c[1], 1000.0f); }
}

// --------------------------------------------------------------------------
//  Constant-per-color field: every interior pixel reconstructs to the exact
//  per-color constants (neighbor averages of identical values are exact).
// --------------------------------------------------------------------------

TEST(Debayer, ConstantPerColorReconstructsExactly) {
    const int w = 8, h = 8;
    std::vector<float> cfa(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int c = color_rggb(x, y);
            cfa[static_cast<size_t>(y) * w + x] = (c == 0) ? 100.0f : (c == 1) ? 200.0f : 50.0f;
        }

    std::vector<float> r, g, b;
    fitsx::debayer_bilinear(cfa, w, h, BayerPattern::RGGB, 0, 0, r, g, b);

    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            EXPECT_NEAR(r[i], 100.0f, 1e-3f) << " at " << x << "," << y;
            EXPECT_NEAR(g[i], 200.0f, 1e-3f) << " at " << x << "," << y;
            EXPECT_NEAR(b[i],  50.0f, 1e-3f) << " at " << x << "," << y;
        }
}

// --------------------------------------------------------------------------
//  Gray-world balance
// --------------------------------------------------------------------------

TEST(Debayer, GrayWorldEqualizesChannelMedians) {
    const size_t n = 4096;
    std::vector<float> r(n, 10.0f), g(n, 40.0f), b(n, 20.0f);
    fitsx::gray_world_balance(r, g, b);
    // After balancing, the three medians should match (ref = mean of medians).
    const float ref = (10.0f + 40.0f + 20.0f) / 3.0f;
    EXPECT_NEAR(r[0], ref, 1e-3f);
    EXPECT_NEAR(g[0], ref, 1e-3f);
    EXPECT_NEAR(b[0], ref, 1e-3f);
}

TEST(Debayer, GrayWorldNoOpOnNonPositiveBackground) {
    const size_t n = 1024;
    std::vector<float> r(n, 0.0f), g(n, 40.0f), b(n, 20.0f);
    fitsx::gray_world_balance(r, g, b);
    // A zero-median channel disables balancing entirely (calibrated frames).
    EXPECT_FLOAT_EQ(r[0], 0.0f);
    EXPECT_FLOAT_EQ(g[0], 40.0f);
    EXPECT_FLOAT_EQ(b[0], 20.0f);
}
