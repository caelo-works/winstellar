#pragma once

#include <cstddef>

namespace fitsx {

// Clockwise rotation of a row-major buffer in 90-degree steps, as a pure index
// map. The index math is deliberately identical to
// viewer/src/InspectRotation.h::rotate_grid_bgra so the exports and the
// inspection overlays can never disagree about orientation -- a sign mistake in
// a second copy is exactly the class of bug that tooling exists to catch.
struct RotMap {
    int src_w = 0, src_h = 0;
    int out_w = 0, out_h = 0;   // swapped for 90/270
    int deg   = 0;              // 0/90/180/270, clockwise

    // Source index for output column `xo`, output row `yo`.
    [[nodiscard]] std::size_t src(int xo, int yo) const noexcept {
        int sr, sc;
        switch (deg) {
            case 90:  sr = src_h - 1 - xo; sc = yo;             break;
            case 180: sr = src_h - 1 - yo; sc = src_w - 1 - xo; break;
            case 270: sr = xo;             sc = src_w - 1 - yo; break;
            default:  sr = yo;             sc = xo;             break;
        }
        return static_cast<std::size_t>(sr) * static_cast<std::size_t>(src_w)
             + static_cast<std::size_t>(sc);
    }
};

[[nodiscard]] inline RotMap make_rot_map(int w, int h, int rotation_deg) noexcept {
    RotMap m;
    m.src_w = w;
    m.src_h = h;
    int d = rotation_deg % 360;
    if (d < 0) d += 360;
    // Anything that is not a right angle is treated as no rotation rather than
    // silently producing a garbage map.
    m.deg = (d == 90 || d == 180 || d == 270) ? d : 0;
    if (m.deg == 90 || m.deg == 270) { m.out_w = h; m.out_h = w; }
    else                             { m.out_w = w; m.out_h = h; }
    return m;
}

// A FITS-native array is stored bottom-up, i.e. it is the vertical mirror of the
// displayed image. Since flip . rot(t) . flip == rot(-t), a display rotation of
// `display_deg` clockwise must be applied to the STORED array as (360 - deg).
// Getting this conjugation backwards writes a file 90 degrees off from what the
// user saw, and no amount of reading the file back catches it -- only a marker
// pixel does.
[[nodiscard]] inline int array_rotation_for_display(int display_deg) noexcept {
    int d = display_deg % 360;
    if (d < 0) d += 360;
    if (d != 90 && d != 180 && d != 270) return 0;
    return (360 - d) % 360;
}

}  // namespace fitsx
