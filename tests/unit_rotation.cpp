#include "InspectRotation.h"   // viewer/src, added to this test's include path

#include <gtest/gtest.h>

#include <set>
#include <vector>

TEST(Rotation, CellMapIdentityAtZero) {
    for (int N : {3, 5})
        for (int dr = 0; dr < N; ++dr)
            for (int dc = 0; dc < N; ++dc) {
                int sr, sc;
                disp_to_src_cell(dr, dc, N, 0, sr, sc);
                EXPECT_EQ(sr, dr);
                EXPECT_EQ(sc, dc);
            }
}

TEST(Rotation, CellMapIsAPermutation) {
    // For every rotation, the displayed->source map must hit each source cell
    // exactly once, otherwise an overlay would drop or double a cell.
    for (int N : {3, 5})
        for (int rot : {0, 90, 180, 270}) {
            std::set<int> seen;
            for (int dr = 0; dr < N; ++dr)
                for (int dc = 0; dc < N; ++dc)
                    seen.insert(disp_to_src_index(dr, dc, N, rot));
            EXPECT_EQ(static_cast<int>(seen.size()), N * N) << "rot=" << rot << " N=" << N;
            EXPECT_EQ(*seen.begin(), 0);
            EXPECT_EQ(*seen.rbegin(), N * N - 1);
        }
}

TEST(Rotation, BgraFourNinetiesIsIdentity) {
    // Rotating a square BGRA buffer by 90 deg four times returns the original.
    const int n = 3;
    std::vector<uint8_t> orig(static_cast<size_t>(n) * n * 4);
    for (size_t i = 0; i < orig.size(); ++i) orig[i] = static_cast<uint8_t>(i * 7 + 1);

    std::vector<uint8_t> a = orig, b;
    for (int k = 0; k < 4; ++k) { rotate_bgra_square(a, n, 90, b); a = b; }
    EXPECT_EQ(a, orig);
}

TEST(Rotation, BgraZeroIsCopyAnd180IsDouble90) {
    const int n = 4;
    std::vector<uint8_t> s(static_cast<size_t>(n) * n * 4);
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>(i);

    std::vector<uint8_t> z;
    rotate_bgra_square(s, n, 0, z);
    EXPECT_EQ(z, s);

    std::vector<uint8_t> once, twice, oneeighty;
    rotate_bgra_square(s, n, 90, once);
    rotate_bgra_square(once, n, 90, twice);
    rotate_bgra_square(s, n, 180, oneeighty);
    EXPECT_EQ(twice, oneeighty);   // 90 twice == 180
}

TEST(Rotation, GridDimsSwapOn90And270) {
    const int R = 2, C = 5;
    std::vector<uint8_t> s(static_cast<size_t>(R) * C * 4, 0);
    std::vector<uint8_t> d;
    int dR = 0, dC = 0;
    rotate_grid_bgra(s, R, C, 90, d, dR, dC);
    EXPECT_EQ(dR, C); EXPECT_EQ(dC, R);
    rotate_grid_bgra(s, R, C, 270, d, dR, dC);
    EXPECT_EQ(dR, C); EXPECT_EQ(dC, R);
    rotate_grid_bgra(s, R, C, 180, d, dR, dC);
    EXPECT_EQ(dR, R); EXPECT_EQ(dC, C);
}

TEST(Rotation, PointIdentityAndCorners) {
    float xo, yo;
    rotate_pt(1.0f, 2.0f, 3, 0, xo, yo);
    EXPECT_FLOAT_EQ(xo, 1.0f); EXPECT_FLOAT_EQ(yo, 2.0f);
    // 90 deg cw in a 3x3 (m=2): (x,y)->(m-y, x)
    rotate_pt(0.0f, 0.0f, 3, 90, xo, yo);
    EXPECT_FLOAT_EQ(xo, 2.0f); EXPECT_FLOAT_EQ(yo, 0.0f);
}
