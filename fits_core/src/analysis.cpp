#include "fits_core/analysis.h"

#include "fits_core/pixmath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fitsx {

namespace {

// ---------------------------------------------------------------------------
//  Pixel stats: single pass for mean/stddev/min/max + their counts; second
//  pass for exact median + MAD via nth_element on copies.
// ---------------------------------------------------------------------------

struct PixelStats {
    double mean = 0.0;
    double stddev = 0.0;
    double median = 0.0;
    double mad = 0.0;
    double min_value = 0.0;
    uint64_t min_count = 0;
    double max_value = 0.0;
    uint64_t max_count = 0;
};

// luma_at() (Rec.601) lives in fits_core/pixmath.h -- shared with the PSF and
// background tools so all three agree on what "brightness" means.

PixelStats compute_pixel_stats(const FitsImage& img) {
    PixelStats s;
    const size_t n = img.pixel_count();
    if (n == 0) return s;

    // Pass 1: Welford for mean/stddev + provisional min/max. Counters are
    // resolved in pass 3 below — the naive single-pass "first-equal" counter
    // only counts pixels matching the *current* extremum (set mid-stream),
    // so a single outlier pixel mid-image would silently zero the real
    // saturation count. Loaders sanitize NaN/Inf so no per-pixel isfinite
    // branch is needed here.
    double mean = 0.0, m2 = 0.0;
    float vmin = std::numeric_limits<float>::infinity();
    float vmax = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < n; ++i) {
        const float v = luma_at(img, i);
        const double dv = v - mean;
        mean += dv / static_cast<double>(i + 1);
        m2 += dv * (v - mean);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    s.mean = mean;
    s.stddev = (n > 1) ? std::sqrt(m2 / static_cast<double>(n - 1)) : 0.0;
    s.min_value = vmin;
    s.max_value = vmax;

    // Pass 2: exact min_count / max_count. A separate pass is what makes the
    // counters correct (and incidentally autovectorizes — both branches are
    // pure comparisons + an increment).
    uint64_t cmin = 0, cmax = 0;
    for (size_t i = 0; i < n; ++i) {
        const float v = luma_at(img, i);
        if (v == vmin) ++cmin;
        if (v == vmax) ++cmax;
    }
    s.min_count = cmin;
    s.max_count = cmax;

    // Pass 3: median + MAD via subsampling. Astro pixel statistics are
    // perceptually identical between a full scan and a ~200k-sample scan
    // unless the image is pathological (which astro data isn't). 100x faster
    // than the previous "copy every finite pixel into work then nth_element"
    // path, and frees ~144 MB of temporary on a 36 Mpx float image.
    constexpr size_t kTargetSamples = 200000;
    const size_t step = std::max<size_t>(1, n / kTargetSamples);
    std::vector<float> work;
    work.reserve(n / step + 1);
    for (size_t i = 0; i < n; i += step) work.push_back(luma_at(img, i));

    if (!work.empty()) {
        auto mid = work.begin() + work.size() / 2;
        std::nth_element(work.begin(), mid, work.end());
        s.median = *mid;

        // Reuse `work` for deviations -- nth_element already partitioned it,
        // but we overwrite each entry so the partition state is irrelevant.
        const float med_f = static_cast<float>(s.median);
        for (size_t i = 0; i < work.size(); ++i) {
            work[i] = std::fabs(work[i] - med_f);
        }
        auto dmid = work.begin() + work.size() / 2;
        std::nth_element(work.begin(), dmid, work.end());
        s.mad = *dmid;
    }
    return s;
}

// ---------------------------------------------------------------------------
//  Star detection (robust). A spatially-adaptive local-background mesh removes
//  nebulosity / gradients / amp-glow (which a single global threshold mistakes
//  for stars), then 8-connected components are filtered by area, roundness,
//  sharpness and SNR. Bright/saturated blobs bypass the shape cuts (after
//  local-background subtraction only a star can be that peaked), so the big
//  obvious stars a global cap used to drop are kept. Tuned on real OSC frames.
// ---------------------------------------------------------------------------

// Detection tunables. Calibrated visually against a Milky-Way OSC frame
// (nebulosity + dense field) so they reject the nebula edge while keeping
// faint *and* saturated stars.
constexpr int    kTileSize   = 64;     // background-mesh cell, px
constexpr double kThrSigma   = 4.0;    // detection = bg + k*noise
constexpr int    kMinPixels  = 5;      // kills hot pixels / single-px noise
constexpr int    kMaxPixels  = 2000;   // generous; shape cuts do the real work
constexpr double kEccMax     = 0.80;   // reject streaks / nebula filaments
constexpr double kFwhmLo     = 1.2;    // reject too-sharp (hot/cosmic)
constexpr double kFwhmHi     = 9.0;    // reject too-diffuse (nebula knots)
constexpr double kSnrMin     = 3.0;    // blob flux significance
constexpr double kSharpMax   = 0.95;   // peak/flux; ~1 == single hot pixel
constexpr double kSnrBright  = 30.0;   // peak/noise above which shape cuts are
                                       // skipped (bright/saturated stars)

struct UnionFind {
    std::vector<int32_t> parent;
    void reserve(size_t n) { parent.reserve(n); }
    int32_t make_set() {
        const int32_t id = static_cast<int32_t>(parent.size());
        parent.push_back(id);
        return id;
    }
    int32_t find(int32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];   // path halving
            x = parent[x];
        }
        return x;
    }
    void unite(int32_t a, int32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[a] = b;
    }
};

double median_of(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    auto m = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}

// Flat per-above-threshold-pixel record. Replaces the per-blob {xs, ys, vs}
// vector-of-vectors so star detection allocates O(detected pixels) rather
// than O(W*H + per-blob heap churn).
struct PixelEntry {
    int32_t x;
    int32_t y;
    float   v;       // residual (luma - local background); always > 0
    int32_t label;   // UF id at insert time; resolved in pass 2 below.
};

// Coarse local background + noise map. Each cell holds a sigma-clipped median
// (background) and a MAD-derived sigma (noise); values are bilinearly
// interpolated between cell centres so the per-pixel threshold varies smoothly.
struct BgMesh {
    int gx = 0, gy = 0, tile = 0;
    std::vector<float> bg;      // gy*gx
    std::vector<float> noise;   // gy*gx

    void sample(float x, float y, float& b, float& n) const {
        float fx = x / tile - 0.5f;
        float fy = y / tile - 0.5f;
        fx = std::clamp(fx, 0.0f, static_cast<float>(gx - 1));
        fy = std::clamp(fy, 0.0f, static_cast<float>(gy - 1));
        const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
        const int i1 = std::min(i0 + 1, gx - 1), j1 = std::min(j0 + 1, gy - 1);
        const float tx = fx - i0, ty = fy - j0;
        auto bilerp = [&](const std::vector<float>& m) {
            const float v00 = m[static_cast<size_t>(j0) * gx + i0];
            const float v10 = m[static_cast<size_t>(j0) * gx + i1];
            const float v01 = m[static_cast<size_t>(j1) * gx + i0];
            const float v11 = m[static_cast<size_t>(j1) * gx + i1];
            return (v00 * (1 - tx) + v10 * tx) * (1 - ty)
                 + (v01 * (1 - tx) + v11 * tx) * ty;
        };
        b = bilerp(bg);
        n = bilerp(noise);
    }
};

// Sigma-clipped {median, 1.4826*MAD} over v (reordered in place); `dev` is a
// reused scratch buffer for the absolute deviations.
std::pair<float, float> clipped_stats(std::vector<float>& v, std::vector<float>& dev) {
    const size_t n = v.size();
    if (n == 0) return {0.0f, 0.0f};
    std::nth_element(v.begin(), v.begin() + n / 2, v.end());
    const float med = v[n / 2];
    dev.resize(n);
    for (size_t i = 0; i < n; ++i) dev[i] = std::fabs(v[i] - med);
    std::nth_element(dev.begin(), dev.begin() + n / 2, dev.end());
    return {med, 1.4826f * dev[n / 2]};
}

BgMesh compute_bg_mesh(const FitsImage& img) {
    const int W = img.width, H = img.height;
    BgMesh m;
    m.tile = kTileSize;
    m.gx = (W + kTileSize - 1) / kTileSize;
    m.gy = (H + kTileSize - 1) / kTileSize;
    m.bg.assign(static_cast<size_t>(m.gx) * m.gy, 0.0f);
    m.noise.assign(static_cast<size_t>(m.gx) * m.gy, 0.0f);

    const bool rgb = img.is_rgb();
    std::vector<float> cell, dev;
    cell.reserve(static_cast<size_t>(kTileSize) * kTileSize);

    for (int j = 0; j < m.gy; ++j) {
        const int y0 = j * kTileSize, y1 = std::min(y0 + kTileSize, H);
        for (int i = 0; i < m.gx; ++i) {
            const int x0 = i * kTileSize, x1 = std::min(x0 + kTileSize, W);
            cell.clear();
            for (int y = y0; y < y1; ++y) {
                const size_t roff = static_cast<size_t>(y) * W;
                for (int x = x0; x < x1; ++x) {
                    const size_t idx = roff + x;
                    cell.push_back(rgb
                        ? (0.299f * img.data[idx] + 0.587f * img.data_g[idx]
                           + 0.114f * img.data_b[idx])
                        : img.data[idx]);
                }
            }
            auto [med, sig] = clipped_stats(cell, dev);
            // Up to 3 high-side sigma-clips so stars don't bias the background.
            for (int it = 0; it < 3 && sig > 0.0f; ++it) {
                const float hi = med + 3.0f * sig;
                auto end = std::partition(cell.begin(), cell.end(),
                                          [hi](float val) { return val < hi; });
                if (static_cast<size_t>(end - cell.begin()) < 8) break;
                cell.erase(end, cell.end());
                std::tie(med, sig) = clipped_stats(cell, dev);
            }
            const size_t ci = static_cast<size_t>(j) * m.gx + i;
            m.bg[ci]    = med;
            m.noise[ci] = (sig > 1e-6f) ? sig : 1e-6f;
        }
    }

    // 3x3 median-smooth the mesh so a cell swamped by a big/saturated star
    // (whose clip couldn't fully recover the sky) doesn't punch a hole.
    auto smooth = [&](std::vector<float>& mesh) {
        std::vector<float> out(mesh.size());
        float w[9];
        for (int j = 0; j < m.gy; ++j) {
            for (int i = 0; i < m.gx; ++i) {
                int c = 0;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) {
                        const int jj = std::clamp(j + dj, 0, m.gy - 1);
                        const int ii = std::clamp(i + di, 0, m.gx - 1);
                        w[c++] = mesh[static_cast<size_t>(jj) * m.gx + ii];
                    }
                std::nth_element(w, w + 4, w + 9);
                out[static_cast<size_t>(j) * m.gx + i] = w[4];
            }
        }
        mesh.swap(out);
    };
    smooth(m.bg);
    smooth(m.noise);
    return m;
}

std::vector<DetectedStar> detect_stars(const FitsImage& img) {
    const int W = img.width;
    const int H = img.height;
    if (W <= 0 || H <= 0) return {};

    const BgMesh mesh = compute_bg_mesh(img);

    // Pass 1: streaming connected-component labeling with a 2-row sliding
    // window, against the spatially-adaptive threshold bg(x,y)+k*noise(x,y).
    // Each kept pixel stores its background-subtracted residual. The full-image
    // label array (was 144 MB on a 36 Mpx image) is replaced by ~8-32 KB of
    // row buffers.
    std::vector<int32_t> prev_row(static_cast<size_t>(W), -1);
    std::vector<int32_t> curr_row(static_cast<size_t>(W), -1);
    UnionFind uf;
    std::vector<PixelEntry> pixels;
    // RGB frames threshold on luma (see luma_at); mono reads `data` directly.
    const bool rgb = img.is_rgb();
    // Loaders sanitize NaN/Inf so the threshold compare is the only branch.
    for (int y = 0; y < H; ++y) {
        const size_t roff = static_cast<size_t>(y) * static_cast<size_t>(W);
        const float* row   = img.data.data()   + roff;
        const float* row_g = rgb ? img.data_g.data() + roff : nullptr;
        const float* row_b = rgb ? img.data_b.data() + roff : nullptr;
        for (int x = 0; x < W; ++x) {
            const float lum = rgb ? (0.299f * row[x] + 0.587f * row_g[x] + 0.114f * row_b[x])
                                  : row[x];
            float b, n;
            mesh.sample(static_cast<float>(x), static_cast<float>(y), b, n);
            if (lum <= b + static_cast<float>(kThrSigma) * n) { curr_row[x] = -1; continue; }

            // 8-connected: NW, N, NE in prev row; W in curr row.
            int32_t best = -1;
            auto consider = [&](int32_t nl) {
                if (nl < 0) return;
                if (best < 0) best = nl;
                else if (best != nl) uf.unite(best, nl);
            };
            if (x > 0)       consider(prev_row[x - 1]);
                              consider(prev_row[x    ]);
            if (x + 1 < W)   consider(prev_row[x + 1]);
            if (x > 0)       consider(curr_row[x - 1]);

            if (best < 0) best = uf.make_set();
            curr_row[x] = best;
            pixels.push_back({ x, y, lum - b, best });   // residual
        }
        std::swap(prev_row, curr_row);
        std::fill(curr_row.begin(), curr_row.end(), -1);
    }

    if (pixels.empty()) return {};

    // Pass 2: resolve UF roots in place.
    for (auto& p : pixels) p.label = uf.find(p.label);

    // Pass 3: count pixels per root, pre-filter on [kMinPixels, kMaxPixels]
    // (the shape/SNR cuts in pass 5 do the real rejection), build
    // offset-into-bucketed-array begins. Rejected blobs are marked begin == -1.
    const size_t n_labels = uf.parent.size();
    std::vector<int32_t> counts(n_labels, 0);
    for (const auto& p : pixels) ++counts[static_cast<size_t>(p.label)];

    std::vector<int32_t> begin(n_labels, -1);
    int32_t kept = 0;
    for (size_t r = 0; r < n_labels; ++r) {
        const int32_t c = counts[r];
        if (c >= kMinPixels && c <= kMaxPixels) {
            begin[r] = kept;
            kept += c;
        } else {
            counts[r] = 0;   // hides the blob from the analyze pass
        }
    }
    if (kept == 0) return {};

    // Pass 4: bucket-sort pixels by their resolved root so each blob's
    // pixels are contiguous. `cursor` walks the per-blob begin and is the
    // counting-sort index pointer.
    std::vector<PixelEntry> bucketed(static_cast<size_t>(kept));
    std::vector<int32_t> cursor = begin;
    for (const auto& p : pixels) {
        const int32_t off = begin[static_cast<size_t>(p.label)];
        if (off < 0) continue;
        bucketed[static_cast<size_t>(cursor[static_cast<size_t>(p.label)]++)] = p;
    }
    // Free the unsorted scratch as early as possible.
    std::vector<PixelEntry>().swap(pixels);
    std::vector<int32_t>().swap(cursor);

    // Pass 5: per-blob analysis. Each blob's pixels live in
    // bucketed[begin[r] .. begin[r] + counts[r]).
    std::vector<DetectedStar> stars;
    stars.reserve(static_cast<size_t>(kept) / 16);

    for (size_t r = 0; r < n_labels; ++r) {
        if (begin[r] < 0) continue;
        PixelEntry* px = bucketed.data() + begin[r];
        const int n = counts[r];

        // px[i].v already holds the background-subtracted residual (> 0).
        double flux = 0.0;
        double peak = 0.0;
        for (int i = 0; i < n; ++i) {
            const double iv = px[i].v;
            flux += iv;
            if (iv > peak) peak = iv;
        }
        if (flux <= 0.0) continue;

        // Flux-weighted centroid.
        double cx = 0.0, cy = 0.0;
        for (int i = 0; i < n; ++i) {
            cx += px[i].x * px[i].v;
            cy += px[i].y * px[i].v;
        }
        cx /= flux;
        cy /= flux;

        // Second moments around centroid.
        double Mxx = 0.0, Myy = 0.0, Mxy = 0.0;
        for (int i = 0; i < n; ++i) {
            const double dx = px[i].x - cx;
            const double dy = px[i].y - cy;
            const double w  = px[i].v;
            Mxx += w * dx * dx;
            Myy += w * dy * dy;
            Mxy += w * dx * dy;
        }
        Mxx /= flux; Myy /= flux; Mxy /= flux;

        const double tr = (Mxx + Myy) * 0.5;
        const double dt = std::sqrt(std::max(0.0,
            (Mxx - Myy) * (Mxx - Myy) * 0.25 + Mxy * Mxy));
        const double l_max = tr + dt;
        const double l_min = std::max(0.0, tr - dt);
        if (l_max <= 0.0) continue;
        const double sigma = std::sqrt((l_max + l_min) * 0.5);
        const double fwhm  = 2.3548200450309493 * sigma;
        const double ecc   = std::sqrt(std::max(0.0, 1.0 - l_min / l_max));
        // Major-axis orientation from the same second moments. atan2 here is in
        // image space (y grows downward), which is exactly what the overlay
        // wants -- the aberration vectors are drawn in that same frame.
        const double theta = 0.5 * std::atan2(2.0 * Mxy, Mxx - Myy);

        // Quality cuts. Local noise at the centroid drives both SNR measures.
        float b_c = 0.0f, n_c = 1.0f;
        mesh.sample(static_cast<float>(cx), static_cast<float>(cy), b_c, n_c);
        const double nloc    = (n_c > 1e-6f) ? n_c : 1e-6;
        const double snr     = flux / (nloc * std::sqrt(static_cast<double>(n)));
        const double sharp   = peak / flux;        // ~1 == single hot pixel
        const double peaksnr = peak / nloc;
        // Bright override: after local-background subtraction only a star can be
        // this peaked, so bright/saturated blobs skip the shape cuts (their flat
        // tops inflate FWHM/ecc). Self-calibrating via the local noise.
        const bool bright = peaksnr >= kSnrBright;

        if (snr < kSnrMin) continue;
        if (!bright) {
            if (ecc > kEccMax) continue;
            if (fwhm < kFwhmLo || fwhm > kFwhmHi) continue;
            if (sharp > kSharpMax) continue;
        }

        // HFR: sort blob pixels by distance from centroid, walk cumulative
        // flux and interpolate between the two pixels straddling 50%.
        struct RP { double r; double w; };
        std::vector<RP> rp(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double dx = px[i].x - cx;
            const double dy = px[i].y - cy;
            rp[i] = { std::sqrt(dx*dx + dy*dy), static_cast<double>(px[i].v) };
        }
        std::sort(rp.begin(), rp.end(),
                  [](const RP& a, const RP& c) { return a.r < c.r; });
        double cumul = 0.0;
        double hfr   = 0.0;
        const double half = flux * 0.5;
        for (int i = 0; i < n; ++i) {
            const double next = cumul + rp[i].w;
            if (next >= half) {
                if (i == 0 || rp[i].w <= 0.0) {
                    hfr = rp[i].r;
                } else {
                    const double frac = (half - cumul) / rp[i].w;
                    hfr = rp[i-1].r + frac * (rp[i].r - rp[i-1].r);
                }
                break;
            }
            cumul = next;
        }
        if (hfr <= 0.0) continue;

        DetectedStar ds;
        ds.x     = static_cast<float>(cx);
        ds.y     = static_cast<float>(cy);
        ds.flux  = static_cast<float>(flux);
        ds.hfr   = static_cast<float>(hfr);
        ds.fwhm  = static_cast<float>(fwhm);
        ds.ecc   = static_cast<float>(ecc);
        ds.theta = static_cast<float>(theta);
        stars.push_back(ds);
    }
    return stars;
}

}  // namespace

AnalysisResult run_analysis(const FitsImage& img) {
    AnalysisResult r;
    if (img.empty()) return r;

    const PixelStats stats = compute_pixel_stats(img);
    r.mean       = stats.mean;
    r.stddev     = stats.stddev;
    r.median     = stats.median;
    r.mad        = stats.mad;
    r.min_value  = stats.min_value;
    r.min_count  = stats.min_count;
    r.max_value  = stats.max_value;
    r.max_count  = stats.max_count;

    {
        const auto stars = detect_stars(img);
        r.star_count = static_cast<int>(stars.size());

        if (!stars.empty()) {
            std::vector<double> hfrs, fwhms, eccs;
            hfrs.reserve(stars.size());
            fwhms.reserve(stars.size());
            eccs.reserve(stars.size());
            for (const auto& s : stars) {
                hfrs.push_back(s.hfr);
                fwhms.push_back(s.fwhm);
                eccs.push_back(s.ecc);
            }
            r.hfr_median         = median_of(hfrs);
            r.fwhm_median        = median_of(fwhms);
            r.eccentricity_median = median_of(eccs);

            // HFR std-dev: classic SD over the star-by-star list.
            double sum = 0.0, sq = 0.0;
            for (double h : hfrs) { sum += h; sq += h * h; }
            const double n = static_cast<double>(hfrs.size());
            const double mean_hfr = sum / n;
            r.hfr_stddev = (n > 1.0)
                ? std::sqrt(std::max(0.0, sq / n - mean_hfr * mean_hfr))
                : 0.0;
        }
    }

    r.success = true;
    return r;
}

DetailedAnalysis run_detailed_analysis(const FitsImage& img) {
    DetailedAnalysis d;
    if (img.empty()) return d;

    d.width  = img.width;
    d.height = img.height;

    // Same detector as run_analysis -- the inspection overlay must agree with
    // the cached star count. background/threshold are informational only here
    // (the detector now uses a spatially-adaptive local threshold internally).
    const PixelStats stats = compute_pixel_stats(img);
    d.background = stats.median;
    d.threshold = stats.median + kThrSigma * 1.4826 * stats.mad;
    d.stars = detect_stars(img);
    d.success = true;
    return d;
}

TiltResult compute_tilt(const DetailedAnalysis& a, int grid) {
    TiltResult t;
    if (!a.success || grid < 2 || a.width <= 0 || a.height <= 0) return t;

    t.grid = grid;
    const size_t ncells = static_cast<size_t>(grid) * static_cast<size_t>(grid);
    t.cells.resize(ncells);

    // Collect per-cell HFR lists, then median each. ecc is meaned (a robust
    // median would need a second pass; the mean is fine for the tilt overlay).
    std::vector<std::vector<double>> hfr_by_cell(ncells);
    std::vector<double> ecc_sum(ncells, 0.0);

    const double gx_scale = static_cast<double>(grid) / a.width;
    const double gy_scale = static_cast<double>(grid) / a.height;
    for (const auto& s : a.stars) {
        int gx = static_cast<int>(s.x * gx_scale);
        int gy = static_cast<int>(s.y * gy_scale);
        if (gx < 0) gx = 0; else if (gx >= grid) gx = grid - 1;
        if (gy < 0) gy = 0; else if (gy >= grid) gy = grid - 1;
        const size_t ci = static_cast<size_t>(gy) * grid + gx;
        hfr_by_cell[ci].push_back(s.hfr);
        ecc_sum[ci] += s.ecc;
    }

    bool any = false;
    double hmin =  std::numeric_limits<double>::infinity();
    double hmax = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < ncells; ++i) {
        TiltCell& c = t.cells[i];
        c.count = static_cast<int>(hfr_by_cell[i].size());
        if (c.count > 0) {
            c.hfr_median = median_of(hfr_by_cell[i]);
            c.ecc_mean   = ecc_sum[i] / c.count;
            hmin = std::min(hmin, c.hfr_median);
            hmax = std::max(hmax, c.hfr_median);
            any = true;
        }
    }
    if (!any) return t;   // no populated cells -> success stays false
    t.hfr_min = hmin;
    t.hfr_max = hmax;

    // Tilt magnitude from the four corner cells (the most diagnostic of a
    // tilted sensor / focuser plane). Only corners that actually hold stars
    // count; if fewer than two corners are populated, tilt_pct stays 0.
    const int corner_idx[4] = {
        0,                                   // TL
        grid - 1,                            // TR
        (grid - 1) * grid,                   // BL
        (grid - 1) * grid + (grid - 1),      // BR
    };
    double cmin =  std::numeric_limits<double>::infinity();
    double cmax = -std::numeric_limits<double>::infinity();
    int populated = 0;
    for (int k = 0; k < 4; ++k) {
        const TiltCell& c = t.cells[static_cast<size_t>(corner_idx[k])];
        if (c.count == 0) continue;
        ++populated;
        if (c.hfr_median < cmin) { cmin = c.hfr_median; t.best_corner = k; }
        if (c.hfr_median > cmax) { cmax = c.hfr_median; t.worst_corner = k; }
    }
    if (populated >= 2 && cmin > 0.0) {
        t.tilt_pct = (cmax - cmin) / cmin * 100.0;
    }

    // Field curvature: how much the corners soften (or sharpen) relative to the
    // centre. Positive => corners worse than centre (classic curvature /
    // backfocus); distinct from tilt (a corner-to-corner asymmetry).
    const TiltCell& centre = t.cells[static_cast<size_t>((grid / 2) * grid + grid / 2)];
    if (populated >= 1 && centre.count > 0 && centre.hfr_median > 0.0) {
        double csum = 0.0; int cn = 0;
        for (int k = 0; k < 4; ++k) {
            const TiltCell& c = t.cells[static_cast<size_t>(corner_idx[k])];
            if (c.count == 0) continue;
            csum += c.hfr_median; ++cn;
        }
        if (cn > 0)
            t.curvature_pct = (csum / cn - centre.hfr_median) / centre.hfr_median * 100.0;
    }

    t.success = true;
    return t;
}

// ---------------------------------------------------------------------------
//  Cache key — FNV-1a 64-bit over (head8K || size_be).
// ---------------------------------------------------------------------------

// Number of leading bytes folded into the cache key. Callers MUST sample the
// same prefix or their keys won't match: the property handler (in Explorer) and
// the viewer's from-file path both hash this many bytes + the true file size, so
// the viewer-populated cache is readable by Explorer (see #34). Kept small so it
// stays O(1) per file when scrolling a folder.
constexpr size_t kCacheKeySampleBytes = 64 * 1024;

std::string compute_cache_key(const void* buffer, size_t size) noexcept {
    constexpr uint64_t kFnvPrime = 0x100000001b3ull;
    // Two decorrelated FNV-1a lanes -> a 128-bit key. A single 64-bit FNV is
    // only ~2^32 (birthday) collision-resistant, which made a targeted prefix+
    // size collision (cache poisoning) plausible; 128 bits raises that bar far
    // out of reach for a display-metadata cache. Lane 2 uses a different seed
    // and a byte transform + rotation so it can't track lane 1.
    uint64_t h1 = 0xcbf29ce484222325ull;
    uint64_t h2 = 0x9e3779b97f4a7c15ull;

    const auto* p = static_cast<const uint8_t*>(buffer);
    const size_t take = std::min<size_t>(size, kCacheKeySampleBytes);
    for (size_t i = 0; i < take; ++i) {
        h1 ^= p[i];
        h1 *= kFnvPrime;
        h2 ^= static_cast<uint8_t>(p[i] ^ 0xA5u) + i;
        h2 *= kFnvPrime;
        h2 = (h2 << 13) | (h2 >> 51);      // rotate for cross-lane diffusion
    }
    // Fold the true size into both lanes (big-endian), content-agnostic suffix.
    for (int sh = 56; sh >= 0; sh -= 8) {
        const uint8_t b = static_cast<uint8_t>((static_cast<uint64_t>(size) >> sh) & 0xFFu);
        h1 ^= b; h1 *= kFnvPrime;
        h2 ^= b; h2 *= kFnvPrime;
    }

    char out[33] = {0};
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) { out[i]      = hex[h1 & 0xFu]; h1 >>= 4; }
    for (int i = 15; i >= 0; --i) { out[16 + i] = hex[h2 & 0xFu]; h2 >>= 4; }
    return std::string(out, 32);
}

std::string compute_cache_key_from_file(const wchar_t* utf16_path) {
    if (!utf16_path) return {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, utf16_path, L"rb") != 0 || !f) return {};
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return {}; }
    const long sz = std::ftell(f);
    if (sz <= 0) { std::fclose(f); return {}; }
    std::fseek(f, 0, SEEK_SET);
    // Sample the same prefix compute_cache_key folds in (kCacheKeySampleBytes),
    // so this key matches the one the Explorer property handler derives for the
    // same file. Heap-allocated (64 KB is too large for the stack).
    std::vector<uint8_t> head(kCacheKeySampleBytes, 0);
    std::fread(head.data(), 1, head.size(), f);
    std::fclose(f);
    return compute_cache_key(head.data(), static_cast<size_t>(sz));
}

}  // namespace fitsx
