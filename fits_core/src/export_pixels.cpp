#include "fits_core/export_pixels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fitsx {

namespace {

// A sample is "already 16-bit" when it is a whole number inside [0, 65535].
// Data that has been bias-subtracted goes negative and fails this, which is the
// point: those frames get the affine path and keep their negatives instead of
// being clipped to zero.
inline bool is_u16_integral(float v) noexcept {
    return std::isfinite(v) && v >= 0.0f && v <= 65535.0f
        && v == std::floor(v);
}

void scan_plane(const std::vector<float>& p, double& lo, double& hi, bool& all_u16) {
    for (const float v : p) {
        if (!std::isfinite(v)) { all_u16 = false; continue; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (all_u16 && !is_u16_integral(v)) all_u16 = false;
    }
}

inline std::uint16_t to_u16(float v, bool identity, double lo, double inv_span) noexcept {
    if (!std::isfinite(v)) return 0;
    if (identity) {
        // Guarded even though compute_linear16_scale proved the range, because
        // a caller could pair a scale with a different image.
        const float c = v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v);
        return static_cast<std::uint16_t>(c);
    }
    const double dn = std::lround(65535.0 * (static_cast<double>(v) - lo) * inv_span);
    if (dn <= 0.0) return 0;
    if (dn >= 65535.0) return 65535;
    return static_cast<std::uint16_t>(dn);
}

}  // namespace

Linear16Scale compute_linear16_scale(const FitsImage& img) {
    Linear16Scale s;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    bool all_u16 = true;

    scan_plane(img.data, lo, hi, all_u16);
    if (img.is_rgb()) {
        scan_plane(img.data_g, lo, hi, all_u16);
        scan_plane(img.data_b, lo, hi, all_u16);
    }

    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        // Nothing finite at all: nothing to scale from.
        s.identity = false;
        s.lo = 0.0;
        s.hi = 1.0;
        s.degenerate_anchors_replaced = true;
        return s;
    }

    s.lo = lo;
    s.hi = hi;
    s.identity = all_u16;
    if (!s.identity && !(hi > lo)) {
        // Constant frame. Any span keeps the mapping defined; the whole image
        // lands on 0.
        s.hi = lo + 1.0;
        s.degenerate_anchors_replaced = true;
    }
    return s;
}

void pack_bgr24_rows(const RenderedBitmap& src, const RotMap& m,
                     int y0, int rows, std::uint8_t* dst) noexcept {
    if (!dst || rows <= 0) return;
    const std::uint8_t* s = src.bgra.data();
    for (int r = 0; r < rows; ++r) {
        const int y = y0 + r;
        if (y < 0 || y >= m.out_h) continue;
        std::uint8_t* out = dst + static_cast<std::size_t>(r) * m.out_w * 3;
        for (int x = 0; x < m.out_w; ++x) {
            const std::size_t si = m.src(x, y) * 4;
            out[0] = s[si + 0];   // B
            out[1] = s[si + 1];   // G
            out[2] = s[si + 2];   // R
            out += 3;
        }
    }
}

void pack_gray16_rows(const FitsImage& img, const RotMap& m, const Linear16Scale& s,
                      int y0, int rows, std::uint16_t* dst) noexcept {
    if (!dst || rows <= 0) return;
    const double inv_span = s.identity ? 0.0 : 1.0 / (s.hi - s.lo);
    const float* p = img.data.data();
    for (int r = 0; r < rows; ++r) {
        const int y = y0 + r;
        if (y < 0 || y >= m.out_h) continue;
        std::uint16_t* out = dst + static_cast<std::size_t>(r) * m.out_w;
        for (int x = 0; x < m.out_w; ++x)
            out[x] = to_u16(p[m.src(x, y)], s.identity, s.lo, inv_span);
    }
}

void pack_rgb48_rows(const FitsImage& img, const RotMap& m, const Linear16Scale& s,
                     int y0, int rows, std::uint16_t* dst) noexcept {
    if (!dst || rows <= 0) return;
    const double inv_span = s.identity ? 0.0 : 1.0 / (s.hi - s.lo);
    const float* pr = img.data.data();
    const float* pg = img.data_g.data();
    const float* pb = img.data_b.data();
    for (int r = 0; r < rows; ++r) {
        const int y = y0 + r;
        if (y < 0 || y >= m.out_h) continue;
        std::uint16_t* out = dst + static_cast<std::size_t>(r) * m.out_w * 3;
        for (int x = 0; x < m.out_w; ++x) {
            const std::size_t si = m.src(x, y);
            out[0] = to_u16(pr[si], s.identity, s.lo, inv_span);
            out[1] = to_u16(pg[si], s.identity, s.lo, inv_span);
            out[2] = to_u16(pb[si], s.identity, s.lo, inv_span);
            out += 3;
        }
    }
}

void rotate_elements(const void* src, const RotMap& m, std::size_t elem_bytes,
                     void* dst) noexcept {
    if (!src || !dst || elem_bytes == 0) return;
    const auto* s = static_cast<const std::uint8_t*>(src);
    auto* d = static_cast<std::uint8_t*>(dst);
    if (m.deg == 0) {
        std::memcpy(d, s, static_cast<std::size_t>(m.src_w) * m.src_h * elem_bytes);
        return;
    }
    for (int y = 0; y < m.out_h; ++y)
        for (int x = 0; x < m.out_w; ++x)
            std::memcpy(d + (static_cast<std::size_t>(y) * m.out_w + x) * elem_bytes,
                        s + m.src(x, y) * elem_bytes,
                        elem_bytes);
}

}  // namespace fitsx
