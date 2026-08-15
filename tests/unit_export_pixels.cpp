// Pixel arithmetic of the export lane: the 16-bit scale decision and the row
// packers. All of it is pure, so it is pinned here rather than being discovered
// later by a user comparing numbers in another tool.

#include "fits_core/export_pixels.h"
#include "fits_core/fits_render.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

using fitsx::FitsImage;
using fitsx::Linear16Scale;
using fitsx::RotMap;
using fitsx::compute_linear16_scale;
using fitsx::make_rot_map;

namespace {

FitsImage mono(int w, int h, std::vector<float> v) {
    FitsImage img;
    img.width = w;
    img.height = h;
    img.data = std::move(v);
    return img;
}

}  // namespace

// ------------------------------------------------------- scale decision ----

TEST(ExportPixels, IntegerDataInRangeIsPassedThroughUnchanged) {
    // The dominant real case: a 16-bit integer source. Rescaling it would make
    // the exported numbers differ from the source for no reason.
    auto img = mono(2, 2, {0.0f, 1.0f, 32768.0f, 65535.0f});
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_TRUE(s.identity);
    EXPECT_FALSE(s.degenerate_anchors_replaced);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    EXPECT_EQ(out, (std::vector<uint16_t>{0, 1, 32768, 65535}));
}

TEST(ExportPixels, FractionalDataUsesTheFramesOwnExtremes) {
    auto img = mono(2, 2, {0.5f, 1.5f, 2.5f, 3.5f});
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_FALSE(s.identity);
    EXPECT_DOUBLE_EQ(s.lo, 0.5);
    EXPECT_DOUBLE_EQ(s.hi, 3.5);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[3], 65535);
    EXPECT_NEAR(out[1], 21845, 1);
    EXPECT_NEAR(out[2], 43690, 1);
}

TEST(ExportPixels, NegativesAndOutOfRangeSurviveInsteadOfClipping) {
    // Calibrated subs go negative and can exceed 65535. Anchoring on the
    // frame's own extremes is what stops that data being silently destroyed.
    auto img = mono(2, 2, {-500.0f, 0.0f, 70000.0f, 100000.0f});
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_FALSE(s.identity) << "out-of-range data must not take the passthrough path";
    EXPECT_DOUBLE_EQ(s.lo, -500.0);
    EXPECT_DOUBLE_EQ(s.hi, 100000.0);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    EXPECT_EQ(out[0], 0);        // the true minimum, not a clipped zero
    EXPECT_EQ(out[3], 65535);
    EXPECT_GT(out[1], 0);        // 0.0 is genuinely above the minimum
    EXPECT_LT(out[2], 65535);
}

TEST(ExportPixels, ConstantFrameDoesNotDivideByZero) {
    auto img = mono(2, 2, {7.25f, 7.25f, 7.25f, 7.25f});
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_TRUE(s.degenerate_anchors_replaced);
    EXPECT_GT(s.hi, s.lo);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4, 0xEEEE);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    for (uint16_t v : out) EXPECT_EQ(v, 0);
}

TEST(ExportPixels, ConstantIntegerFrameStaysIdentity) {
    // A constant frame of whole numbers is still passthrough: the loader's
    // degenerate [0,1] substitution must not leak in and rescale it.
    auto img = mono(2, 2, {1000.0f, 1000.0f, 1000.0f, 1000.0f});
    img.source_min = 0.0;   // what the loader writes when the range degenerates
    img.source_max = 1.0;
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_TRUE(s.identity);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    for (uint16_t v : out) EXPECT_EQ(v, 1000);
}

TEST(ExportPixels, NonFiniteSamplesDoNotPoisonTheRange) {
    auto img = mono(2, 2, {std::nanf(""), 10.5f, 20.5f,
                           std::numeric_limits<float>::infinity()});
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_FALSE(s.identity);
    EXPECT_DOUBLE_EQ(s.lo, 10.5);
    EXPECT_DOUBLE_EQ(s.hi, 20.5);

    const RotMap m = make_rot_map(2, 2, 0);
    std::vector<uint16_t> out(4);
    fitsx::pack_gray16_rows(img, m, s, 0, 2, out.data());
    EXPECT_EQ(out[0], 0);   // NaN lands at zero rather than an arbitrary value
    EXPECT_EQ(out[3], 0);
}

// -------------------------------------------------------------- packers ----

TEST(ExportPixels, Bgr24DropsAlphaAndKeepsChannelOrder) {
    fitsx::RenderedBitmap rb;
    rb.width = 2;
    rb.height = 1;
    rb.stride_bytes = 8;
    rb.bgra = {1, 2, 3, 99, 4, 5, 6, 88};   // B,G,R,A per pixel

    const RotMap m = make_rot_map(2, 1, 0);
    std::vector<uint8_t> out(6, 0);
    fitsx::pack_bgr24_rows(rb, m, 0, 1, out.data());
    EXPECT_EQ(out, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
}

TEST(ExportPixels, PackersHonourTheRotation) {
    // A corner marker is the only thing that catches a transposed map.
    auto img = mono(3, 2, {9.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f});
    const Linear16Scale s = compute_linear16_scale(img);
    ASSERT_TRUE(s.identity);

    const RotMap m = make_rot_map(3, 2, 90);
    ASSERT_EQ(m.out_w, 2);
    ASSERT_EQ(m.out_h, 3);
    std::vector<uint16_t> out(6, 0);
    fitsx::pack_gray16_rows(img, m, s, 0, 3, out.data());

    // Top-left of a 3x2 rotated 90 clockwise lands top-right of the 2x3 result.
    EXPECT_EQ(out[1], 9) << "rotation is wrong or transposed";
    EXPECT_EQ(out[0], 0);
}

TEST(ExportPixels, RowBlocksTileTheSameAsOnePass) {
    // The streaming path must produce exactly what a single call would.
    const int W = 5, H = 4;
    std::vector<float> v(static_cast<size_t>(W) * H);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<float>(i * 37 % 1000);
    auto img = mono(W, H, v);
    const Linear16Scale s = compute_linear16_scale(img);
    const RotMap m = make_rot_map(W, H, 270);

    std::vector<uint16_t> whole(static_cast<size_t>(m.out_w) * m.out_h);
    fitsx::pack_gray16_rows(img, m, s, 0, m.out_h, whole.data());

    std::vector<uint16_t> blocked(whole.size(), 0xFFFF);
    const int kBlock = 2;
    for (int y = 0; y < m.out_h; y += kBlock) {
        const int n = std::min(kBlock, m.out_h - y);
        fitsx::pack_gray16_rows(img, m, s, y, n,
                                blocked.data() + static_cast<size_t>(y) * m.out_w);
    }
    EXPECT_EQ(blocked, whole);
}

TEST(ExportPixels, Rgb48KeepsPlaneOrderRGB) {
    FitsImage img;
    img.width = 1;
    img.height = 1;
    img.data   = {11.0f};
    img.data_g = {22.0f};
    img.data_b = {33.0f};
    ASSERT_TRUE(img.is_rgb());

    const Linear16Scale s = compute_linear16_scale(img);
    ASSERT_TRUE(s.identity);
    const RotMap m = make_rot_map(1, 1, 0);
    std::vector<uint16_t> out(3, 0);
    fitsx::pack_rgb48_rows(img, m, s, 0, 1, out.data());
    EXPECT_EQ(out, (std::vector<uint16_t>{11, 22, 33}));
}

TEST(ExportPixels, Rgb48ScaleSpansAllThreePlanes) {
    // One shared scale, or the channels come out with different gains and the
    // colour balance shifts.
    FitsImage img;
    img.width = 1;
    img.height = 1;
    img.data   = {0.5f};
    img.data_g = {1.5f};
    img.data_b = {2.5f};
    const Linear16Scale s = compute_linear16_scale(img);
    EXPECT_FALSE(s.identity);
    EXPECT_DOUBLE_EQ(s.lo, 0.5);
    EXPECT_DOUBLE_EQ(s.hi, 2.5);
}

TEST(ExportPixels, Bgr24MatchesTheRendererOutput) {
    // Proves the JPG/PNG lane really is "what render_to_bgra produced", minus
    // alpha -- the only mechanical link we can assert without a window.
    auto img = mono(4, 3, {});
    img.data.resize(12);
    for (size_t i = 0; i < img.data.size(); ++i) img.data[i] = static_cast<float>(i) / 11.0f;
    img.source_min = 0.0;
    img.source_max = 1.0;

    fitsx::StretchParams sp;
    const fitsx::RenderedBitmap rb = fitsx::render_to_bgra(img, sp);
    ASSERT_EQ(rb.width, 4);
    ASSERT_EQ(rb.height, 3);

    const RotMap m = make_rot_map(rb.width, rb.height, 0);
    std::vector<uint8_t> packed(static_cast<size_t>(4) * 3 * 3);
    fitsx::pack_bgr24_rows(rb, m, 0, 3, packed.data());

    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(packed[i * 3 + 0], rb.bgra[i * 4 + 0]);
        EXPECT_EQ(packed[i * 3 + 1], rb.bgra[i * 4 + 1]);
        EXPECT_EQ(packed[i * 3 + 2], rb.bgra[i * 4 + 2]);
    }
}

// ------------------------------------------------------ native rotation ----

TEST(ExportPixels, RotateElementsMovesNativeSamples) {
    const uint16_t src[6] = {1, 2, 3, 4, 5, 6};   // 3x2
    const RotMap m = make_rot_map(3, 2, 180);
    uint16_t dst[6] = {};
    fitsx::rotate_elements(src, m, sizeof(uint16_t), dst);
    const uint16_t want[6] = {6, 5, 4, 3, 2, 1};
    for (int i = 0; i < 6; ++i) EXPECT_EQ(dst[i], want[i]);
}

TEST(ExportPixels, RotateElementsHandlesEveryNativeWidth) {
    for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        std::vector<uint8_t> src(6 * w), dst(6 * w, 0);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i + 1);
        const RotMap m = make_rot_map(3, 2, 90);
        fitsx::rotate_elements(src.data(), m, w, dst.data());
        // Source element 0 (3x2, rot 90) lands at output (x=1, y=0).
        for (std::size_t b = 0; b < w; ++b)
            EXPECT_EQ(dst[w + b], src[b]) << "elem_bytes=" << w;
    }
}

TEST(ExportPixels, RotateElementsZeroIsAPlainCopy) {
    const uint32_t src[4] = {10, 20, 30, 40};
    const RotMap m = make_rot_map(2, 2, 0);
    uint32_t dst[4] = {};
    fitsx::rotate_elements(src, m, sizeof(uint32_t), dst);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(dst[i], src[i]);
}
