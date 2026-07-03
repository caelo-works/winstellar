#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Rotation permutation shared by the inspection popups (Tilt / Aberration /
// Background). Each maps the *displayed* orientation back to the source grid so
// the popup follows the main view's clockwise rotation (0 / 90 / 180 / 270).
// These used to be re-implemented (slightly differently) in each view; a sign
// mistake in one copy would make the overlays disagree about orientation --
// exactly the class of bug these tools exist to diagnose. Single source now.

// Displayed cell (dr,dc) in an N x N grid -> source cell (sr,sc).
inline void disp_to_src_cell(int dr, int dc, int N, int rot, int& sr, int& sc) {
    switch (rot) {
        case 90:  sr = N - 1 - dc; sc = dr;         break;
        case 180: sr = N - 1 - dr; sc = N - 1 - dc; break;
        case 270: sr = dc;         sc = N - 1 - dr; break;
        default:  sr = dr;         sc = dc;         break;
    }
}

// Same, as a flat row-major source index into an N x N grid.
inline int disp_to_src_index(int dr, int dc, int N, int rot) {
    int sr, sc;
    disp_to_src_cell(dr, dc, N, rot, sr, sc);
    return sr * N + sc;
}

// Rotate an R x C (rows x cols) BGRA buffer clockwise by `rot`. Output dims land
// in dR x dC (swapped for 90/270). rot==0 is a straight copy.
inline void rotate_grid_bgra(const std::vector<uint8_t>& s, int R, int C, int rot,
                             std::vector<uint8_t>& d, int& dR, int& dC) {
    if (rot == 0) { d = s; dR = R; dC = C; return; }
    if (rot == 180) { dR = R; dC = C; } else { dR = C; dC = R; }
    d.resize(static_cast<size_t>(dR) * dC * 4);
    for (int i = 0; i < dR; ++i)
        for (int j = 0; j < dC; ++j) {
            int sr, sc;
            switch (rot) {
                case 90:  sr = R - 1 - j; sc = i;         break;
                case 180: sr = R - 1 - i; sc = C - 1 - j; break;
                case 270: sr = j;         sc = C - 1 - i; break;
                default:  sr = i;         sc = j;         break;
            }
            std::memcpy(&d[(static_cast<size_t>(i) * dC + j) * 4],
                        &s[(static_cast<size_t>(sr) * C + sc) * 4], 4);
        }
}

// Square convenience wrapper (n x n -> n x n).
inline void rotate_bgra_square(const std::vector<uint8_t>& s, int n, int rot,
                               std::vector<uint8_t>& d) {
    int dR = 0, dC = 0;
    rotate_grid_bgra(s, n, n, rot, d, dR, dC);
}

// Rotate a point (x,y) within an n x n grid clockwise by `rot`.
inline void rotate_pt(float x, float y, int n, int rot, float& xo, float& yo) {
    const float m = n - 1.0f;
    switch (rot) {
        case 90:  xo = m - y; yo = x;     break;
        case 180: xo = m - x; yo = m - y; break;
        case 270: xo = y;     yo = m - x; break;
        default:  xo = x;     yo = y;     break;
    }
}
