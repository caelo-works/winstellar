#include "fits_core/fits_debayer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace fitsx {

namespace {

// Color codes used internally: 0 = Red, 1 = Green, 2 = Blue.
constexpr int kR = 0, kG = 1, kB = 2;

// 2x2 color tile for a pattern, indexed [row & 1][col & 1] in FITS-native
// (un-flipped) order, matching the BAYERPAT letter reading order TL,TR,BL,BR.
struct Tile { int c[2][2]; };

Tile tile_for(BayerPattern p) noexcept {
    switch (p) {
        case BayerPattern::RGGB: return { { {kR, kG}, {kG, kB} } };
        case BayerPattern::BGGR: return { { {kB, kG}, {kG, kR} } };
        case BayerPattern::GRBG: return { { {kG, kR}, {kB, kG} } };
        case BayerPattern::GBRG: return { { {kG, kB}, {kR, kG} } };
        default:                 return { { {kG, kG}, {kG, kG} } };
    }
}

std::string upper_trim(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\'' || ch == '"' || std::isspace(static_cast<unsigned char>(ch)))
            continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

// Subsampled median of a plane, ~100k samples. Returns NaN if no finite data.
float plane_median(const std::vector<float>& v) {
    if (v.empty()) return std::numeric_limits<float>::quiet_NaN();
    constexpr size_t kTarget = 100000;
    const size_t step = std::max<size_t>(1, v.size() / kTarget);
    std::vector<float> work;
    work.reserve(v.size() / step + 1);
    for (size_t i = 0; i < v.size(); i += step) work.push_back(v[i]);
    if (work.empty()) return std::numeric_limits<float>::quiet_NaN();
    auto mid = work.begin() + work.size() / 2;
    std::nth_element(work.begin(), mid, work.end());
    return *mid;
}

}  // namespace

BayerPattern parse_bayer_pattern(const std::string& s) noexcept {
    const std::string u = upper_trim(s);
    if (u == "RGGB") return BayerPattern::RGGB;
    if (u == "BGGR") return BayerPattern::BGGR;
    if (u == "GRBG") return BayerPattern::GRBG;
    if (u == "GBRG") return BayerPattern::GBRG;
    return BayerPattern::None;
}

BayerPattern detect_bayer_pattern(const FitsImage& img, int& xoff, int& yoff) noexcept {
    std::string s;
    if (!img.find_header("BAYERPAT", s)) return BayerPattern::None;
    const BayerPattern p = parse_bayer_pattern(s);
    if (p == BayerPattern::None) return BayerPattern::None;

    // Pattern-origin offsets are optional; default 0. Different capture
    // software spells them XBAYROFF/YBAYROFF (ASI/INDI) or BAYOFFX/BAYOFFY.
    long v = 0;
    if (img.find_header_int("XBAYROFF", v) || img.find_header_int("BAYOFFX", v))
        xoff = static_cast<int>(((v % 2) + 2) % 2);
    if (img.find_header_int("YBAYROFF", v) || img.find_header_int("BAYOFFY", v))
        yoff = static_cast<int>(((v % 2) + 2) % 2);
    return p;
}

void debayer_bilinear(const std::vector<float>& cfa, int w, int h,
                      BayerPattern pat, int xoff, int yoff,
                      std::vector<float>& r,
                      std::vector<float>& g,
                      std::vector<float>& b) {
    const size_t npix = static_cast<size_t>(w) * static_cast<size_t>(h);
    r.assign(npix, 0.0f);
    g.assign(npix, 0.0f);
    b.assign(npix, 0.0f);
    if (pat == BayerPattern::None || w <= 0 || h <= 0 || cfa.size() < npix) return;

    const Tile tile = tile_for(pat);
    const int ox = ((xoff % 2) + 2) % 2;
    const int oy = ((yoff % 2) + 2) % 2;
    auto color_at = [&](int x, int y) -> int {
        return tile.c[(y + oy) & 1][(x + ox) & 1];
    };

    // Bilinear demosaic. For each pixel we keep its native-color sample and
    // reconstruct the two missing channels by averaging the 8-neighborhood
    // samples that carry each color. The Bayer geometry guarantees this picks
    // exactly the classic neighbor sets (4 edge-neighbors for green at an R/B
    // site, 4 diagonals for the opposite color, the 2 axis-neighbors for a
    // color at a green site), so one neighbor scan handles every case. Out-of-
    // bounds neighbors are skipped, biasing only a 1-2 px border.
    for (int y = 0; y < h; ++y) {
        const int y0 = (y > 0) ? y - 1 : 0;
        const int y1 = (y + 1 < h) ? y + 1 : h - 1;
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + x;
            const int self_c = color_at(x, y);
            const float self_v = cfa[idx];

            const int x0 = (x > 0) ? x - 1 : 0;
            const int x1 = (x + 1 < w) ? x + 1 : w - 1;

            float sum[3] = {0, 0, 0};
            int   cnt[3] = {0, 0, 0};
            for (int ny = y0; ny <= y1; ++ny) {
                for (int nx = x0; nx <= x1; ++nx) {
                    if (nx == x && ny == y) continue;
                    const int c = color_at(nx, ny);
                    sum[c] += cfa[static_cast<size_t>(ny) * static_cast<size_t>(w) + nx];
                    ++cnt[c];
                }
            }

            float out[3];
            for (int c = 0; c < 3; ++c) {
                if (c == self_c)      out[c] = self_v;
                else if (cnt[c] > 0)  out[c] = sum[c] / static_cast<float>(cnt[c]);
                else                  out[c] = self_v;   // degenerate edge fallback
            }
            r[idx] = out[kR];
            g[idx] = out[kG];
            b[idx] = out[kB];
        }
    }
}

void gray_world_balance(std::vector<float>& r,
                        std::vector<float>& g,
                        std::vector<float>& b) {
    const float mr = plane_median(r);
    const float mg = plane_median(g);
    const float mb = plane_median(b);
    // Need three positive backgrounds to define meaningful channel gains.
    // Calibrated (bias-subtracted) frames can sit near zero -- skip rather
    // than amplify noise into a false cast.
    if (!(mr > 0.0f) || !(mg > 0.0f) || !(mb > 0.0f)) return;

    const float ref = (mr + mg + mb) / 3.0f;
    const float gr = std::clamp(ref / mr, 0.2f, 5.0f);
    const float gg = std::clamp(ref / mg, 0.2f, 5.0f);
    const float gb = std::clamp(ref / mb, 0.2f, 5.0f);

    for (float& v : r) v *= gr;
    for (float& v : g) v *= gg;
    for (float& v : b) v *= gb;
}

}  // namespace fitsx
