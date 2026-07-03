#include "fits_core/background.h"

#include "fits_core/pixmath.h"
#include "fits_core/stats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fitsx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int    kRows = 48, kCols = 72;   // GRID_ROWS, GRID_COLS
constexpr int    kRings = 12;

// quantile / median / mad_sigma live in fits_core/stats.h.

// Solve a small weighted least squares c = argmin |W(A c - y)| via normal
// equations (k x k Gaussian elimination). rows = basis vectors, keep = mask.
std::vector<double> solve_lsq(const std::vector<std::vector<double>>& A,
                              const std::vector<double>& y,
                              const std::vector<char>& keep, int k) {
    std::vector<std::vector<double>> M(k, std::vector<double>(k + 1, 0.0));  // [ATA | ATy]
    for (size_t r = 0; r < A.size(); ++r) {
        if (!keep[r]) continue;
        const auto& a = A[r];
        for (int i = 0; i < k; ++i) {
            for (int j = 0; j < k; ++j) M[i][j] += a[i] * a[j];
            M[i][k] += a[i] * y[r];
        }
    }
    // Gaussian elimination with partial pivoting.
    for (int col = 0; col < k; ++col) {
        int piv = col;
        for (int r = col + 1; r < k; ++r)
            if (std::abs(M[r][col]) > std::abs(M[piv][col])) piv = r;
        if (std::abs(M[piv][col]) < 1e-15) return std::vector<double>(k, 0.0);
        std::swap(M[col], M[piv]);
        for (int r = 0; r < k; ++r) {
            if (r == col) continue;
            const double f = M[r][col] / M[col][col];
            for (int c = col; c <= k; ++c) M[r][c] -= f * M[col][c];
        }
    }
    std::vector<double> c(k);
    for (int i = 0; i < k; ++i) c[i] = M[i][k] / M[i][i];
    return c;
}

}  // namespace

BackgroundMap compute_background(const FitsImage& img, int min_side) {
    BackgroundMap bg;
    if (img.empty()) return bg;
    const int H = img.height, L = img.width;
    if (std::min(H, L) < min_side) { bg.skipped = true; return bg; }

    const int R = kRows, C = kCols;
    const int by = H / R, bx = L / C;
    if (by < 1 || bx < 1) { bg.skipped = true; return bg; }

    // --- per-cell 25th-percentile sky floor ---
    std::vector<double> B0(static_cast<size_t>(R) * C, 0.0);
    std::vector<float> cell;
    cell.reserve(static_cast<size_t>(by) * bx);
    for (int i = 0; i < R; ++i) {
        for (int j = 0; j < C; ++j) {
            cell.clear();
            const int y0 = i * by, x0 = j * bx;
            for (int y = y0; y < y0 + by; ++y)
                for (int x = x0; x < x0 + bx; ++x)
                    cell.push_back(luma_at(img, x, y));
            B0[static_cast<size_t>(i) * C + j] = quantile_inplace(cell, 0.25);
        }
    }

    // --- normalized coords (centre 0, |corner| ~ 1) ---
    std::vector<double> xn(B0.size()), yn(B0.size()), r(B0.size());
    double rmax = 0.0;
    for (int i = 0; i < R; ++i)
        for (int j = 0; j < C; ++j) {
            const size_t k = static_cast<size_t>(i) * C + j;
            yn[k] = (i - (R - 1) / 2.0) / ((R - 1) / 2.0);
            xn[k] = (j - (C - 1) / 2.0) / ((C - 1) / 2.0);
            r[k] = std::hypot(xn[k], yn[k]);
            rmax = std::max(rmax, r[k]);
        }
    if (rmax <= 0) rmax = 1.0;
    for (auto& v : r) v /= rmax;

    // --- mini-ABE: order-2 polynomial surface, unilateral upward rejection ---
    // terms (i,j) with i+j<=2: (0,0)(0,1)(0,2)(1,0)(1,1)(2,0) -> 6
    constexpr int K = 6;
    std::vector<std::vector<double>> A(B0.size(), std::vector<double>(K));
    for (size_t k = 0; k < B0.size(); ++k) {
        const double X = xn[k], Y = yn[k];
        A[k] = { 1.0, Y, Y * Y, X, X * Y, X * X };
    }
    std::vector<char> keep(B0.size(), 1);
    std::vector<double> coef(K, 0.0);
    for (int it = 0; it < 5; ++it) {
        coef = solve_lsq(A, B0, keep, K);
        std::vector<double> rk;
        for (size_t k = 0; k < B0.size(); ++k)
            if (keep[k]) rk.push_back(B0[k] - (A[k][0]*coef[0] + A[k][1]*coef[1]
                + A[k][2]*coef[2] + A[k][3]*coef[3] + A[k][4]*coef[4] + A[k][5]*coef[5]));
        const double sig = mad_sigma(rk);
        std::vector<char> nk(B0.size(), 0);
        int kept = 0;
        for (size_t k = 0; k < B0.size(); ++k) {
            const double pred = A[k][0]*coef[0] + A[k][1]*coef[1] + A[k][2]*coef[2]
                              + A[k][3]*coef[3] + A[k][4]*coef[4] + A[k][5]*coef[5];
            if (B0[k] - pred < 2.5 * sig) { nk[k] = 1; ++kept; }
        }
        if (kept < K * 2) break;
        const bool same = (nk == keep);
        keep.swap(nk);
        if (same) break;
    }
    std::vector<double> surface(B0.size());
    for (size_t k = 0; k < B0.size(); ++k)
        surface[k] = A[k][0]*coef[0] + A[k][1]*coef[1] + A[k][2]*coef[2]
                   + A[k][3]*coef[3] + A[k][4]*coef[4] + A[k][5]*coef[5];

    bg.rows = R; bg.cols = C;
    bg.surface.assign(surface.begin(), surface.end());
    const double bg_med = median_copy(surface);
    bg.bg_median = bg_med ? bg_med : 1e-6;

    // --- radial profile (rings) on the surface ---
    std::vector<double> prof(kRings, std::nan("")), ring_std(kRings, std::nan(""));
    for (int ri = 0; ri < kRings; ++ri) {
        const double e0 = static_cast<double>(ri) / kRings;
        const double e1 = static_cast<double>(ri + 1) / kRings + (ri + 1 == kRings ? 1e-9 : 0);
        std::vector<double> vals;
        for (size_t k = 0; k < surface.size(); ++k)
            if (r[k] >= e0 && r[k] < e1) vals.push_back(surface[k]);
        if (vals.size() >= 3) {
            prof[ri] = median_copy(vals);
            double m = 0; for (double v : vals) m += v; m /= vals.size();
            double s = 0; for (double v : vals) s += (v - m) * (v - m);
            ring_std[ri] = std::sqrt(s / vals.size());
        }
    }
    double i_center = bg.bg_median, i_corner = bg.bg_median;
    for (double p : prof) if (std::isfinite(p)) { i_center = p; break; }
    for (auto it = prof.rbegin(); it != prof.rend(); ++it) if (std::isfinite(*it)) { i_corner = *it; break; }
    bg.radial_drop = (i_center != 0.0) ? (i_center - i_corner) / i_center : 0.0;
    { double su = 0; int n = 0; for (double s : ring_std) if (std::isfinite(s)) { su += s; ++n; }
      bg.azim_aniso = (n ? su / n : 0.0) / (std::abs(bg.bg_median) + 1e-9); }

    // radial fit c0 + c1 r^2 + c2 r^4 (for r2 only)
    {
        std::vector<std::vector<double>> Ar(surface.size(), std::vector<double>(3));
        std::vector<char> all(surface.size(), 1);
        for (size_t k = 0; k < surface.size(); ++k)
            Ar[k] = { 1.0, r[k]*r[k], r[k]*r[k]*r[k]*r[k] };
        auto cr = solve_lsq(Ar, surface, all, 3);
        double ssr = 0, sst = 0, mean = bg.bg_median;
        for (size_t k = 0; k < surface.size(); ++k) {
            const double pred = cr[0] + cr[1]*Ar[k][1] + cr[2]*Ar[k][2];
            ssr += (surface[k]-pred)*(surface[k]-pred);
            sst += (surface[k]-mean)*(surface[k]-mean);
        }
        bg.radial_r2 = 1.0 - ssr / (sst > 0 ? sst : 1e-12);
    }

    // --- plane fit a*xn + b*yn + c (gradient) ---
    {
        std::vector<std::vector<double>> Ap(surface.size(), std::vector<double>(3));
        std::vector<char> all(surface.size(), 1);
        for (size_t k = 0; k < surface.size(); ++k) Ap[k] = { xn[k], yn[k], 1.0 };
        auto cp = solve_lsq(Ap, surface, all, 3);
        const double a = cp[0], b = cp[1], c = cp[2];
        double ssr = 0, sst = 0, mean = bg.bg_median;
        double cmin = 1e30, cmax = -1e30;
        for (double sx : {-1.0, 1.0}) for (double sy : {-1.0, 1.0}) {
            const double v = a*sx + b*sy + c; cmin = std::min(cmin, v); cmax = std::max(cmax, v);
        }
        for (size_t k = 0; k < surface.size(); ++k) {
            const double pred = a*xn[k] + b*yn[k] + c;
            ssr += (surface[k]-pred)*(surface[k]-pred);
            sst += (surface[k]-mean)*(surface[k]-mean);
        }
        bg.plane_r2 = 1.0 - ssr / (sst > 0 ? sst : 1e-12);
        bg.plane_amplitude_rel = (cmax - cmin) / bg.bg_median;
        double dir = std::fmod(std::atan2(b, a) * 180.0 / kPi, 360.0);
        if (dir < 0) dir += 360.0;
        bg.plane_direction_deg = dir;
    }

    // --- corner excess (amp glow) : B0 - surface residual at the 4 corners ---
    {
        std::vector<double> resid(surface.size());
        for (size_t k = 0; k < surface.size(); ++k) resid[k] = B0[k] - surface[k];
        const double sg = mad_sigma(resid);
        // patch median around relative (ry,rx); frac 0.16 of the grid.
        const int hy = std::max(1, static_cast<int>(R * 0.16 / 2));
        const int hx = std::max(1, static_cast<int>(C * 0.16 / 2));
        auto patch_excess = [&](double ry, double rx) {
            const int cy = static_cast<int>(ry * (R - 1)), cx = static_cast<int>(rx * (C - 1));
            std::vector<double> v;
            for (int y = std::max(0, cy-hy); y < std::min(R, cy+hy+1); ++y)
                for (int x = std::max(0, cx-hx); x < std::min(C, cx+hx+1); ++x)
                    v.push_back(resid[static_cast<size_t>(y)*C + x]);
            return median_copy(v) / sg;
        };
        const double corners[4] = { patch_excess(0,0), patch_excess(0,1),
                                    patch_excess(1,0), patch_excess(1,1) };
        bg.corner_excess_max = *std::max_element(corners, corners + 4);
        // corner asymmetry of the surface levels at the corners
        std::vector<double> cs;
        for (auto rc : {std::pair<double,double>{0,0}, {0,1}, {1,0}, {1,1}}) {
            const int cy = static_cast<int>(rc.first * (R - 1)), cx = static_cast<int>(rc.second * (C - 1));
            std::vector<double> v;
            for (int y = std::max(0, cy-hy); y < std::min(R, cy+hy+1); ++y)
                for (int x = std::max(0, cx-hx); x < std::min(C, cx+hx+1); ++x)
                    v.push_back(surface[static_cast<size_t>(y)*C + x]);
            cs.push_back(median_copy(v));
        }
        double mean = 0; for (double v : cs) mean += v; mean /= cs.size();
        double s = 0; for (double v : cs) s += (v - mean) * (v - mean);
        bg.corner_asymmetry = std::sqrt(s / cs.size()) / (std::abs(mean) + 1e-9);
    }

    bg.success = true;
    return bg;
}

}  // namespace fitsx
