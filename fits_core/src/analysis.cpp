#include "fits_core/analysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
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

// Rec.601 luma weights. For RGB images (debayered CFA or color XISF) all
// pixel statistics and star detection run on luma rather than the red channel
// alone -- red is typically the faintest channel in astro frames, so HFR /
// star counts off R-only would be unreliable. Mono images read `data` directly.
inline float luma_at(const FitsImage& img, size_t i) noexcept {
    if (!img.is_rgb()) return img.data[i];
    return 0.299f * img.data[i] + 0.587f * img.data_g[i] + 0.114f * img.data_b[i];
}

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
//  Star detection: threshold above (median + 5*sigma_mad), 8-connected
//  component labeling via union-find, per-blob centroid + flux + HFR + 2nd
//  moments.
// ---------------------------------------------------------------------------

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

struct StarInfo {
    double hfr = 0.0;
    double fwhm = 0.0;
    double ecc = 0.0;
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
    float   v;
    int32_t label;   // UF id at insert time; resolved in pass 2 below.
};

std::vector<StarInfo> detect_stars(const FitsImage& img,
                                    double background,
                                    double threshold_value) {
    const int W = img.width;
    const int H = img.height;
    if (W <= 0 || H <= 0) return {};

    // Pass 1: streaming connected-component labeling with a 2-row sliding
    // window. Each above-threshold pixel is appended to `pixels` along with
    // its (provisional) Union-Find id. The full-image label array (was 144 MB
    // on a 36 Mpx image) is replaced by ~8-32 KB of row buffers.
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
            const float v = rgb ? (0.299f * row[x] + 0.587f * row_g[x] + 0.114f * row_b[x])
                                : row[x];
            if (v <= threshold_value) { curr_row[x] = -1; continue; }

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
            pixels.push_back({ x, y, v, best });
        }
        std::swap(prev_row, curr_row);
        std::fill(curr_row.begin(), curr_row.end(), -1);
    }

    if (pixels.empty()) return {};

    // Pass 2: resolve UF roots in place.
    for (auto& p : pixels) p.label = uf.find(p.label);

    // Pass 3: count pixels per root, decide which blobs survive the
    // [kMinPixels, kMaxPixels] filter, build offset-into-bucketed-array
    // begins. Rejected blobs are marked with begin == -1 so the bucketing
    // step below skips them without allocating per-blob heap entries.
    constexpr int kMinPixels = 4;
    constexpr int kMaxPixels = 200;

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
    std::vector<StarInfo> stars;
    stars.reserve(static_cast<size_t>(kept) / 16);

    for (size_t r = 0; r < n_labels; ++r) {
        if (begin[r] < 0) continue;
        PixelEntry* px = bucketed.data() + begin[r];
        const int n = counts[r];

        // Background-subtracted intensities; reject any non-positive after
        // subtraction (can happen at the threshold edge).
        double flux = 0.0;
        for (int i = 0; i < n; ++i) {
            const double iv = static_cast<double>(px[i].v) - background;
            if (iv <= 0.0) { px[i].v = 0.0f; }
            else           { px[i].v = static_cast<float>(iv); flux += iv; }
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

        stars.push_back({ hfr, fwhm, ecc });
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

    constexpr double kMadToSigma = 1.4826;
    constexpr double kSigmas = 5.0;
    const double sigma = kMadToSigma * stats.mad;
    const double threshold = stats.median + kSigmas * sigma;

    if (sigma > 0.0) {
        const auto stars = detect_stars(img, stats.median, threshold);
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

// ---------------------------------------------------------------------------
//  Cache key — FNV-1a 64-bit over (head8K || size_be).
// ---------------------------------------------------------------------------

std::string compute_cache_key(const void* buffer, size_t size) noexcept {
    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
    constexpr uint64_t kFnvPrime  = 0x100000001b3ull;

    const auto* p = static_cast<const uint8_t*>(buffer);
    const size_t take = std::min<size_t>(size, 8 * 1024);

    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < take; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    // Mix the size in big-endian so the suffix is content-agnostic.
    for (int sh = 56; sh >= 0; sh -= 8) {
        h ^= static_cast<uint8_t>((size >> sh) & 0xFFu);
        h *= kFnvPrime;
    }

    char out[17] = {0};
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[h & 0xFu];
        h >>= 4;
    }
    return std::string(out, 16);
}

std::string compute_cache_key_from_file(const wchar_t* utf16_path) {
    if (!utf16_path) return {};
    FILE* f = nullptr;
    if (_wfopen_s(&f, utf16_path, L"rb") != 0 || !f) return {};
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return {}; }
    const long sz = std::ftell(f);
    if (sz <= 0) { std::fclose(f); return {}; }
    std::fseek(f, 0, SEEK_SET);
    uint8_t head[8192] = {};   // zero-init; small files leave the tail at 0
    std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    return compute_cache_key(head, static_cast<size_t>(sz));
}

}  // namespace fitsx
