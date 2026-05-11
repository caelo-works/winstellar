#include "fits_core/xisf_loader.h"
#include "helpers/synth_xisf.h"

#include <gtest/gtest.h>

#include <vector>

using fitsx::parse_xisf_header;
using fitsx::load_xisf_from_memory;

TEST(XisfHeader, ParsesGeometryAndKeywords) {
    std::vector<float> pix(16 * 8, 42.0f);
    auto buf = wst::make_synth_xisf(16, 8, pix, "OBJECT", "'NGC 7000'");
    ASSERT_FALSE(buf.empty());

    auto pr = parse_xisf_header(buf.data(), buf.size());
    ASSERT_TRUE(pr.success) << pr.error;
    EXPECT_EQ(pr.header.width,  16);
    EXPECT_EQ(pr.header.height, 8);
    EXPECT_EQ(pr.header.channels, 1);
    EXPECT_FALSE(pr.header.color_rgb);
    EXPECT_FALSE(pr.header.compressed);
    ASSERT_EQ(pr.header.fits_keywords.size(), 1u);
    EXPECT_EQ(pr.header.fits_keywords[0].key,   "OBJECT");
    EXPECT_EQ(pr.header.fits_keywords[0].value, "NGC 7000");
}

TEST(XisfHeader, FailsOnMissingMagic) {
    const std::vector<uint8_t> buf(64, 0);   // all zeros
    auto pr = parse_xisf_header(buf.data(), buf.size());
    EXPECT_FALSE(pr.success);
    EXPECT_NE(pr.error.find("XISF"), std::string::npos);
}

TEST(XisfHeader, FailsOnTruncatedHeader) {
    std::vector<float> pix(4 * 4, 1.0f);
    auto buf = wst::make_synth_xisf(4, 4, pix);
    // Chop the buffer to just past the XML-length prefix; the parser should
    // detect that the XML body itself is missing.
    buf.resize(18);
    auto pr = parse_xisf_header(buf.data(), buf.size());
    EXPECT_FALSE(pr.success);
}

TEST(XisfLoad, LoadsThreeRgbPlanes) {
    // 2x2 RGB image: each plane gets a distinguishing constant value.
    const int W = 2, H = 2;
    std::vector<float> r(W * H,  100.0f);
    std::vector<float> g(W * H,  500.0f);
    std::vector<float> b(W * H, 1000.0f);
    auto buf = wst::make_synth_xisf_rgb(W, H, r, g, b);
    ASSERT_FALSE(buf.empty());

    auto res = load_xisf_from_memory(buf.data(), buf.size());
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_TRUE(res.image.is_rgb());
    EXPECT_EQ(res.image.data.size(),   4u);
    EXPECT_EQ(res.image.data_g.size(), 4u);
    EXPECT_EQ(res.image.data_b.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(res.image.data[i],    100.0f);
        EXPECT_FLOAT_EQ(res.image.data_g[i],  500.0f);
        EXPECT_FLOAT_EQ(res.image.data_b[i], 1000.0f);
    }
    // Global range spans all three planes.
    EXPECT_DOUBLE_EQ(res.image.source_min,  100.0);
    EXPECT_DOUBLE_EQ(res.image.source_max, 1000.0);
}

TEST(XisfLoad, EndToEndMonoFloat32) {
    // Build a 4x4 image where each pixel == its 0-based index, then verify
    // the pixel data round-trips through load_xisf_from_memory.
    std::vector<float> pix(16);
    for (int i = 0; i < 16; ++i) pix[i] = static_cast<float>(i);
    auto buf = wst::make_synth_xisf(4, 4, pix);
    auto r = load_xisf_from_memory(buf.data(), buf.size());
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.image.width,  4);
    EXPECT_EQ(r.image.height, 4);
    ASSERT_EQ(r.image.data.size(), 16u);
    // XISF is top-down (no flip), so data[i] == i.
    for (int i = 0; i < 16; ++i) {
        EXPECT_FLOAT_EQ(r.image.data[i], static_cast<float>(i)) << "i=" << i;
    }
    EXPECT_DOUBLE_EQ(r.image.source_min, 0.0);
    EXPECT_DOUBLE_EQ(r.image.source_max, 15.0);
}
