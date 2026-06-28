#include "fits_core/psf.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fitsx {

namespace {

constexpr double kPi       = 3.14159265358979323846;
constexpr int    kCut      = kPsfStamp;   // 29
constexpr double kSnrMin   = 15.0;
constexpr double kConcMin  = 0.12;
constexpr int    kLocalK   = 7;           // local-maximum window
constexpr double kDetSig   = 10.0;        // detect threshold = med + 10*sigma

inline float luma_at(const FitsImage& img, int x, int y) {
    const size_t i = static_cast<size_t>(y) * img.width + x;
    if (!img.is_rgb()) return img.data[i];
    return 0.299f * img.data[i] + 0.587f * img.data_g[i] + 0.114f * img.data_b[i];
}

double median_inplace(std::vector<float>& v) {
    if (v.empty()) return 0.0;
    auto m = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}

// Block-mean local background, subtracted and clipped >= 0 (his highpass()).
void highpass(const std::vector<float>& region, int h, int w, std::vector<float>& hp) {
    const int by = std::max(h / 12, 1), bx = std::max(w / 12, 1);
    const int nbY = std::max(1, h / by), nbX = std::max(1, w / bx);
    std::vector<double> bm(static_cast<size_t>(nbY) * nbX, 0.0);
    std::vector<int>    bc(static_cast<size_t>(nbY) * nbX, 0);
    for (int y = 0; y < h; ++y) {
        const int byi = std::min(y / by, nbY - 1);
        for (int x = 0; x < w; ++x) {
            const int bxi = std::min(x / bx, nbX - 1);
            const size_t b = static_cast<size_t>(byi) * nbX + bxi;
            bm[b] += region[static_cast<size_t>(y) * w + x];
            bc[b] += 1;
        }
    }
    for (size_t b = 0; b < bm.size(); ++b) bm[b] /= std::max(1, bc[b]);
    hp.resize(static_cast<size_t>(h) * w);
    for (int y = 0; y < h; ++y) {
        const int byi = std::min(y / by, nbY - 1);
        for (int x = 0; x < w; ++x) {
            const int bxi = std::min(x / bx, nbX - 1);
            const float v = region[static_cast<size_t>(y) * w + x]
                          - static_cast<float>(bm[static_cast<size_t>(byi) * nbX + bxi]);
            hp[static_cast<size_t>(y) * w + x] = v > 0.0f ? v : 0.0f;
        }
    }
}

struct Moments { float ecc, elong, pa, fwhm, x0, y0, siga, sigb; bool ok; };

// Adaptive Gaussian-weighted second moments (his adaptive_moments). `sub` is the
// background-subtracted, clipped (>=0) stamp of size n x n.
Moments adaptive_moments(const std::vector<float>& sub, int n, int iters = 12) {
    Moments r{}; r.ok = false;
    double s0 = 0.0, sx = 0.0, sy = 0.0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double v = sub[static_cast<size_t>(y) * n + x];
            s0 += v; sx += v * x; sy += v * y;
        }
    if (s0 <= 0.0) return r;
    double x0 = sx / s0, y0 = sy / s0;
    double Qxx = 2.0, Qyy = 2.0, Qxy = 0.0;

    for (int it = 0; it < iters; ++it) {
        const double det = Qxx * Qyy - Qxy * Qxy;
        if (det <= 1e-6) return r;
        // weight covariance = 2*Q; invert it.
        const double a = Qxx * 2, b = Qxy * 2, c = Qyy * 2;
        const double idet = a * c - b * b;
        if (std::abs(idet) < 1e-12) return r;
        const double iwxx = c / idet, iwxy = -b / idet, iwyy = a / idet;

        double s = 0, mx = 0, my = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dx = x - x0, dy = y - y0;
                const double W = std::exp(-0.5 * (iwxx * dx * dx + 2 * iwxy * dx * dy + iwyy * dy * dy));
                const double wi = W * sub[static_cast<size_t>(y) * n + x];
                s += wi; mx += wi * x; my += wi * y;
            }
        if (s <= 0.0) return r;
        const double x0n = mx / s, y0n = my / s;
        double nxx = 0, nyy = 0, nxy = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double dxw = x - x0, dyw = y - y0;   // weight uses prior centroid
                const double W = std::exp(-0.5 * (iwxx * dxw * dxw + 2 * iwxy * dxw * dyw + iwyy * dyw * dyw));
                const double wi = W * sub[static_cast<size_t>(y) * n + x];
                const double dx = x - x0n, dy = y - y0n;
                nxx += wi * dx * dx; nyy += wi * dy * dy; nxy += wi * dx * dy;
            }
        nxx /= s; nyy /= s; nxy /= s;
        const bool conv = std::abs(nxx - Qxx) + std::abs(nyy - Qyy) + std::abs(nxy - Qxy) < 1e-3;
        Qxx = nxx; Qyy = nyy; Qxy = nxy; x0 = x0n; y0 = y0n;
        if (conv) break;
    }

    const double comm = std::sqrt(std::max(((Qxx - Qyy) / 2) * ((Qxx - Qyy) / 2) + Qxy * Qxy, 0.0));
    const double l1 = (Qxx + Qyy) / 2 + comm;
    const double l2 = std::max((Qxx + Qyy) / 2 - comm, 1e-6);
    if (l1 <= 0.0) return r;
    const double q = std::sqrt(l2 / l1);
    r.pa    = static_cast<float>(0.5 * std::atan2(2 * Qxy, Qxx - Qyy));
    r.ecc   = static_cast<float>(std::sqrt(std::max(1 - q * q, 0.0)));
    r.elong = static_cast<float>(1.0 / q);
    r.fwhm  = static_cast<float>(2.3548 * std::sqrt((l1 + l2) / 2));
    r.x0    = static_cast<float>(x0);
    r.y0    = static_cast<float>(y0);
    r.siga  = static_cast<float>(std::sqrt(l1));
    r.sigb  = static_cast<float>(std::sqrt(l2));
    r.ok = true;
    return r;
}

// Quality gate on a candidate at region-local (y,x). Mirrors his is_star().
bool is_star(const std::vector<float>& region, int h, int w, int y, int x, double& score) {
    constexpr int r = 9;
    if (y - r < 0 || x - r < 0 || y + r + 1 > h || x + r + 1 > w) return false;
    const int win = 2 * r + 1;
    std::vector<float> w_buf(static_cast<size_t>(win) * win);
    std::vector<float> annulus;
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) {
            const float v = region[static_cast<size_t>(y + j) * w + (x + i)];
            w_buf[static_cast<size_t>(j + r) * win + (i + r)] = v;
            const double rad = std::sqrt(static_cast<double>(i) * i + static_cast<double>(j) * j);
            if (rad >= 6.0 && rad <= 8.5) annulus.push_back(v);
        }
    if (annulus.empty()) return false;
    std::vector<float> ann2 = annulus;
    const double bg = median_inplace(annulus);
    for (auto& v : ann2) v = std::abs(v - static_cast<float>(bg));
    double noise = 1.4826 * median_inplace(ann2);
    if (noise <= 0.0) noise = 1e-6;

    const double peak = w_buf[static_cast<size_t>(r) * win + r] - bg;
    if (peak <= 0.0 || peak / noise < kSnrMin) return false;

    const double nb = (w_buf[(r - 1) * win + r] + w_buf[(r + 1) * win + r]
                     + w_buf[r * win + (r - 1)] + w_buf[r * win + (r + 1)]) / 4.0 - bg;
    if (nb / peak < 0.35) return false;   // hot pixel / cosmic

    std::vector<float> sub(static_cast<size_t>(win) * win);
    double tot = 0.0;
    for (size_t i = 0; i < sub.size(); ++i) {
        const double v = w_buf[i] - bg;
        sub[i] = v > 0.0 ? static_cast<float>(v) : 0.0f;
        tot += sub[i];
    }
    if (tot <= 0.0) return false;
    double core3 = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            core3 += sub[static_cast<size_t>(r + j) * win + (r + i)];
    if (core3 / tot < kConcMin) return false;

    const Moments m = adaptive_moments(sub, win);
    if (!m.ok || !(m.fwhm >= 1.4f && m.fwhm <= 12.0f)) return false;
    score = peak / noise;
    return true;
}

// Circular median of axis angles (mod pi), his circ_pa().
float circ_pa(const std::vector<float>& pas) {
    double ss = 0, cc = 0;
    for (float p : pas) { ss += std::sin(2.0 * p); cc += std::cos(2.0 * p); }
    return static_cast<float>(0.5 * std::atan2(ss / pas.size(), cc / pas.size()));
}

}  // namespace

PsfPlate compute_psf_plate(const FitsImage& img, int grid, int nstar) {
    PsfPlate plate;
    if (img.empty() || grid < 1 || nstar < 1) return plate;
    const int L = img.width, H = img.height;
    plate.grid = grid; plate.nstar = nstar; plate.width = L; plate.height = H;
    plate.zones.resize(static_cast<size_t>(grid) * grid);

    // Saturation cutoff from the peak luma (his percentile*0.92, simplified).
    float lmax = 0.0f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < L; ++x) lmax = std::max(lmax, luma_at(img, x, y));
    const float sat = lmax * 0.92f;

    const int shortSide = std::min(L, H);
    const int SR = std::max(2 * kCut, static_cast<int>(0.20 * shortSide));
    const int M  = static_cast<int>(0.03 * shortSide);

    auto axis_centre = [](int i, int n, int len, int margin, int sr) {
        const int first = margin + sr / 2, last = len - margin - sr / 2;
        if (n <= 1 || last <= first) return len / 2;
        return first + i * (last - first) / (n - 1);
    };

    std::vector<float> region, hp, stamp;
    for (int gy = 0; gy < grid; ++gy) {
        for (int gx = 0; gx < grid; ++gx) {
            PsfZone& zone = plate.zones[static_cast<size_t>(gy) * grid + gx];
            zone.gx = gx; zone.gy = gy;

            const int cx = axis_centre(gx, grid, L, M, SR);
            const int cy = axis_centre(gy, grid, H, M, SR);
            const int sx0 = std::clamp(cx - SR / 2, 0, std::max(0, L - SR));
            const int sy0 = std::clamp(cy - SR / 2, 0, std::max(0, H - SR));
            const int rw = std::min(SR, L), rh = std::min(SR, H);

            region.resize(static_cast<size_t>(rw) * rh);
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x)
                    region[static_cast<size_t>(y) * rw + x] = luma_at(img, sx0 + x, sy0 + y);

            highpass(region, rh, rw, hp);
            std::vector<float> tmp = hp;
            const double med = median_inplace(tmp);
            std::vector<float> dev = hp;
            for (auto& v : dev) v = std::abs(v - static_cast<float>(med));
            double sig = 1.4826 * median_inplace(dev);
            if (sig <= 0.0) sig = 1e-6;
            const double thr = med + kDetSig * sig;

            // Local maxima (k x k), thresholded, below saturation, away from edge.
            const int off = kLocalK / 2;
            struct Cand { double score; int y, x; };
            std::vector<Cand> cands;
            for (int y = off; y < rh - off; ++y) {
                for (int x = off; x < rw - off; ++x) {
                    const float v = hp[static_cast<size_t>(y) * rw + x];
                    if (v <= thr) continue;
                    if (region[static_cast<size_t>(y) * rw + x] >= sat) continue;
                    if (y < kCut || x < kCut || y > rh - kCut || x > rw - kCut) continue;
                    bool ismax = true;
                    for (int j = -off; j <= off && ismax; ++j)
                        for (int i = -off; i <= off; ++i)
                            if (hp[static_cast<size_t>(y + j) * rw + (x + i)] > v) { ismax = false; break; }
                    if (!ismax) continue;
                    double score = 0.0;
                    if (is_star(region, rh, rw, y, x, score)) cands.push_back({score, y, x});
                }
            }
            std::sort(cands.begin(), cands.end(),
                      [](const Cand& a, const Cand& b) { return a.score > b.score; });

            // Pick top nstar, spaced by 1.3*CUT, then measure on a CUT stamp.
            const double mind2 = (kCut * 1.3) * (kCut * 1.3);
            std::vector<float> elongs, eccs, pas;
            for (const auto& cd : cands) {
                if (static_cast<int>(zone.stars.size()) >= nstar) break;
                const int gyy = sy0 + cd.y, gxx = sx0 + cd.x;
                bool far = true;
                for (const auto& s : zone.stars)
                    if (static_cast<double>(s.cy - gyy) * (s.cy - gyy)
                      + static_cast<double>(s.cx - gxx) * (s.cx - gxx) < mind2) { far = false; break; }
                if (!far) continue;

                const int ox = std::clamp(gxx - kCut / 2, 0, std::max(0, L - kCut));
                const int oy = std::clamp(gyy - kCut / 2, 0, std::max(0, H - kCut));
                stamp.resize(static_cast<size_t>(kCut) * kCut);
                for (int y = 0; y < kCut; ++y)
                    for (int x = 0; x < kCut; ++x)
                        stamp[static_cast<size_t>(y) * kCut + x] = luma_at(img, ox + x, oy + y);
                std::vector<float> tmp2 = stamp;
                const double smed = median_inplace(tmp2);
                std::vector<float> ssub(static_cast<size_t>(kCut) * kCut);
                for (size_t i = 0; i < ssub.size(); ++i) {
                    const double v = stamp[i] - smed;
                    ssub[i] = v > 0.0 ? static_cast<float>(v) : 0.0f;
                }
                const Moments m = adaptive_moments(ssub, kCut);
                if (!m.ok) continue;

                PsfStar st;
                st.cx = gxx; st.cy = gyy; st.ox = ox; st.oy = oy;
                st.x0 = m.x0; st.y0 = m.y0; st.fwhm = m.fwhm;
                st.ecc = m.ecc; st.elong = m.elong; st.pa = m.pa;
                st.siga = m.siga; st.sigb = m.sigb;
                st.snr = static_cast<float>(cd.score);
                zone.stars.push_back(st);
                elongs.push_back(m.elong); eccs.push_back(m.ecc); pas.push_back(m.pa);
            }

            if (!elongs.empty()) {
                std::vector<float> e = elongs, c = eccs;
                zone.elong_median = static_cast<float>(median_inplace(e));
                zone.ecc_median   = static_cast<float>(median_inplace(c));
                zone.pa_median    = circ_pa(pas);
                // Classify vs the radial axis (image centre -> zone centre).
                const double radial = std::atan2(cy - H / 2.0, cx - L / 2.0);
                double dpa = std::fmod(std::abs(zone.pa_median - radial + kPi / 2), kPi);
                dpa = std::abs(dpa - kPi / 2);   // 0..pi/2, distance to radial
                const bool is_centre = (gx == grid / 2 && gy == grid / 2);
                if (is_centre)                      zone.axis = PsfAxis::Ref;
                else if (dpa < 25.0 * kPi / 180.0)  zone.axis = PsfAxis::Radial;
                else if (dpa > 65.0 * kPi / 180.0)  zone.axis = PsfAxis::Tangential;
                else                                zone.axis = PsfAxis::Oblique;
            } else {
                zone.axis = PsfAxis::None;
            }
        }
    }
    plate.success = true;
    return plate;
}

}  // namespace fitsx
