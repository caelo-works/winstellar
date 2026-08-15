// Geometry the export lane depends on: the shared rotation map, and what a
// rotation does to a Bayer pattern.
//
// Both are the kind of thing that fails silently. A rotation map that disagrees
// with the viewer writes a file at the wrong orientation; a Bayer pattern that
// is not rotated with its mosaic produces a file that opens fine and demosaics
// with swapped colours. Neither is visible by reading the output back, so both
// are pinned here against an independent reference.

#include "fits_core/image_rotate.h"
#include "fits_core/fits_debayer.h"

#include "InspectRotation.h"   // viewer/src, the already-shipped reference

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using fitsx::RotMap;
using fitsx::make_rot_map;
using fitsx::array_rotation_for_display;
using fitsx::BayerPattern;

namespace {

std::vector<uint8_t> ramp_bgra(int w, int h) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            v[i + 0] = static_cast<uint8_t>(x);
            v[i + 1] = static_cast<uint8_t>(y);
            v[i + 2] = static_cast<uint8_t>((x * 7 + y * 13) & 0xFF);
            v[i + 3] = 255;
        }
    return v;
}

// Apply RotMap to a BGRA buffer, the way the packers will.
std::vector<uint8_t> rotate_via_map(const std::vector<uint8_t>& src, const RotMap& m) {
    std::vector<uint8_t> out(static_cast<size_t>(m.out_w) * m.out_h * 4);
    for (int y = 0; y < m.out_h; ++y)
        for (int x = 0; x < m.out_w; ++x) {
            const size_t s = m.src(x, y) * 4;
            const size_t d = (static_cast<size_t>(y) * m.out_w + x) * 4;
            for (int c = 0; c < 4; ++c) out[d + c] = src[s + c];
        }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- RotMap ----

TEST(ExportGeometry, RotMapMatchesTheViewersRotation) {
    // Non-square on purpose: a width/height mix-up survives a square test.
    for (int rot : {0, 90, 180, 270}) {
        const int W = 7, H = 4;
        const auto src = ramp_bgra(W, H);

        std::vector<uint8_t> ref;
        int dR = 0, dC = 0;
        rotate_grid_bgra(src, H, W, rot, ref, dR, dC);   // (rows, cols)

        const RotMap m = make_rot_map(W, H, rot);
        EXPECT_EQ(m.out_w, dC) << "rot=" << rot;
        EXPECT_EQ(m.out_h, dR) << "rot=" << rot;

        const auto got = rotate_via_map(src, m);
        EXPECT_EQ(got, ref) << "rot=" << rot << ": export rotation disagrees with the viewer";
    }
}

TEST(ExportGeometry, RotMapDimensionsSwapOnQuarterTurns) {
    EXPECT_EQ(make_rot_map(7, 4, 0).out_w, 7);
    EXPECT_EQ(make_rot_map(7, 4, 0).out_h, 4);
    EXPECT_EQ(make_rot_map(7, 4, 180).out_w, 7);
    EXPECT_EQ(make_rot_map(7, 4, 180).out_h, 4);
    EXPECT_EQ(make_rot_map(7, 4, 90).out_w, 4);
    EXPECT_EQ(make_rot_map(7, 4, 90).out_h, 7);
    EXPECT_EQ(make_rot_map(7, 4, 270).out_w, 4);
    EXPECT_EQ(make_rot_map(7, 4, 270).out_h, 7);
}

TEST(ExportGeometry, FourQuarterTurnsReturnTheOriginal) {
    const int W = 5, H = 3;
    auto buf = ramp_bgra(W, H);
    const auto original = buf;
    int w = W, h = H;
    for (int i = 0; i < 4; ++i) {
        const RotMap m = make_rot_map(w, h, 90);
        buf = rotate_via_map(buf, m);
        w = m.out_w;
        h = m.out_h;
    }
    EXPECT_EQ(w, W);
    EXPECT_EQ(h, H);
    EXPECT_EQ(buf, original);
}

TEST(ExportGeometry, DegenerateStripsAndOddAngles) {
    for (int rot : {0, 90, 180, 270}) {
        EXPECT_NO_THROW({ const auto m = make_rot_map(1, 6, rot); rotate_via_map(ramp_bgra(1, 6), m); });
        EXPECT_NO_THROW({ const auto m = make_rot_map(6, 1, rot); rotate_via_map(ramp_bgra(6, 1), m); });
    }
    // Anything that is not a right angle degrades to "no rotation" rather than
    // producing an out-of-range map.
    EXPECT_EQ(make_rot_map(4, 3, 45).deg, 0);
    EXPECT_EQ(make_rot_map(4, 3, 360).deg, 0);
    EXPECT_EQ(make_rot_map(4, 3, -90).deg, 270);
}

TEST(ExportGeometry, ArrayRotationIsTheDisplayRotationConjugated) {
    // flip . rot(t) . flip == rot(-t): the stored array turns the other way.
    EXPECT_EQ(array_rotation_for_display(0), 0);
    EXPECT_EQ(array_rotation_for_display(90), 270);
    EXPECT_EQ(array_rotation_for_display(180), 180);
    EXPECT_EQ(array_rotation_for_display(270), 90);
}

// ----------------------------------------------------------- Bayer tiles ----

namespace {

// Independent reference: build the mosaic's colour grid, rotate the GRID with
// the viewer's own helper, then read the 2x2 tile straight off the result. This
// derives the answer a completely different way from rotate_bayer_pattern,
// which computes it from index arithmetic alone.
std::string rotated_tile_by_construction(BayerPattern pat, int xoff, int yoff,
                                         int w, int h, int display_deg) {
    // Colour letter of every source pixel, in stored order.
    auto letter_at = [&](int x, int y) -> char {
        // Mirrors fits_debayer.cpp's tile lookup.
        static const char* tiles[] = { "", "RGGB", "BGGR", "GRBG", "GBRG" };
        const char* t = tiles[static_cast<int>(pat)];
        const int r = (y + ((yoff % 2) + 2) % 2) & 1;
        const int c = (x + ((xoff % 2) + 2) % 2) & 1;
        return t[r * 2 + c];
    };

    // Pack one letter per pixel into a 4-byte cell so the BGRA helper can move it.
    std::vector<uint8_t> grid(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            grid[(static_cast<size_t>(y) * w + x) * 4] =
                static_cast<uint8_t>(letter_at(x, y));

    std::vector<uint8_t> out;
    int dR = 0, dC = 0;
    rotate_grid_bgra(grid, h, w, array_rotation_for_display(display_deg), out, dR, dC);

    std::string tile;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c)
            tile.push_back(static_cast<char>(out[(static_cast<size_t>(r) * dC + c) * 4]));
    return tile;
}

}  // namespace

TEST(ExportGeometry, RotatedBayerPatternMatchesTheRotatedMosaic) {
    // Exhaustive over pattern x offsets x parities x rotation. Odd dimensions
    // shift the tile phase, which is exactly why this is computed and not a
    // lookup table.
    const int dims[][2] = { {6, 4}, {5, 4}, {6, 5}, {5, 5} };
    int checked = 0;
    for (BayerPattern pat : { BayerPattern::RGGB, BayerPattern::BGGR,
                              BayerPattern::GRBG, BayerPattern::GBRG }) {
        for (int xoff = 0; xoff < 2; ++xoff) {
            for (int yoff = 0; yoff < 2; ++yoff) {
                for (const auto& d : dims) {
                    for (int rot : {0, 90, 180, 270}) {
                        const BayerPattern got =
                            fitsx::rotate_bayer_pattern(pat, xoff, yoff, d[0], d[1], rot);
                        const std::string want =
                            rotated_tile_by_construction(pat, xoff, yoff, d[0], d[1], rot);
                        ASSERT_NE(got, BayerPattern::None)
                            << "pattern=" << fitsx::bayer_pattern_name(pat)
                            << " off=" << xoff << "," << yoff
                            << " dims=" << d[0] << "x" << d[1] << " rot=" << rot;
                        EXPECT_EQ(std::string(fitsx::bayer_pattern_name(got)), want)
                            << "pattern=" << fitsx::bayer_pattern_name(pat)
                            << " off=" << xoff << "," << yoff
                            << " dims=" << d[0] << "x" << d[1] << " rot=" << rot;
                        ++checked;
                    }
                }
            }
        }
    }
    EXPECT_EQ(checked, 4 * 2 * 2 * 4 * 4);
}

TEST(ExportGeometry, RotationZeroIsIdentityForEveryPattern) {
    for (BayerPattern pat : { BayerPattern::RGGB, BayerPattern::BGGR,
                              BayerPattern::GRBG, BayerPattern::GBRG }) {
        EXPECT_EQ(fitsx::rotate_bayer_pattern(pat, 0, 0, 8, 6, 0), pat);
    }
}

TEST(ExportGeometry, NonePatternStaysNone) {
    EXPECT_EQ(fitsx::rotate_bayer_pattern(BayerPattern::None, 0, 0, 8, 6, 90),
              BayerPattern::None);
    EXPECT_STREQ(fitsx::bayer_pattern_name(BayerPattern::None), "");
}

TEST(ExportGeometry, OffsetsAreFoldedIntoTheResult) {
    // An x-offset of 1 on RGGB is GRBG read from the pattern origin; folding it
    // in is what lets the exporter write XBAYROFF = 0.
    EXPECT_EQ(fitsx::rotate_bayer_pattern(BayerPattern::RGGB, 1, 0, 8, 6, 0),
              BayerPattern::GRBG);
    EXPECT_EQ(fitsx::rotate_bayer_pattern(BayerPattern::RGGB, 0, 1, 8, 6, 0),
              BayerPattern::GBRG);
    EXPECT_EQ(fitsx::rotate_bayer_pattern(BayerPattern::RGGB, 1, 1, 8, 6, 0),
              BayerPattern::BGGR);
}
