#include "fits_core/fits_render.h"

#include <algorithm>
#include <cmath>

namespace fitsx {

namespace {

inline uint8_t to_u8(float v) noexcept {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

}  // namespace

RenderedBitmap render_to_bgra(const FitsImage& img,
                              const StretchParams& stretch,
                              int max_width,
                              int max_height) {
    RenderedBitmap out;
    if (img.empty()) return out;

    int target_w = img.width;
    int target_h = img.height;

    if (max_width > 0 && max_height > 0 &&
        (target_w > max_width || target_h > max_height)) {
        const double sx = static_cast<double>(max_width) / img.width;
        const double sy = static_cast<double>(max_height) / img.height;
        const double s = std::min(sx, sy);
        target_w = std::max(1, static_cast<int>(std::lround(img.width * s)));
        target_h = std::max(1, static_cast<int>(std::lround(img.height * s)));
    }

    out.width = target_w;
    out.height = target_h;
    out.stride_bytes = target_w * 4;
    out.bgra.assign(static_cast<size_t>(out.stride_bytes) *
                        static_cast<size_t>(target_h), 0);

    const double range = img.source_max - img.source_min;
    if (range <= 0.0) return out;

    const float shadows = stretch.shadows;
    const float highlights = stretch.highlights;
    const float midtone = stretch.midtone;
    const float denom = std::max(highlights - shadows, 1e-9f);
    const float src_min = static_cast<float>(img.source_min);
    const float frange = static_cast<float>(range);

    const double scale_x = static_cast<double>(img.width) / target_w;
    const double scale_y = static_cast<double>(img.height) / target_h;

    for (int y = 0; y < target_h; ++y) {
        const int y0 = std::min(img.height - 1, static_cast<int>(y * scale_y));
        const int y1 = std::min(img.height,
                                std::max(y0 + 1, static_cast<int>((y + 1) * scale_y)));
        uint8_t* row = out.bgra.data() +
                       static_cast<size_t>(y) * static_cast<size_t>(out.stride_bytes);

        for (int x = 0; x < target_w; ++x) {
            const int x0 = std::min(img.width - 1, static_cast<int>(x * scale_x));
            const int x1 = std::min(img.width,
                                    std::max(x0 + 1, static_cast<int>((x + 1) * scale_x)));

            double sum = 0.0;
            int count = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const float* src = img.data.data() +
                                   static_cast<size_t>(yy) * static_cast<size_t>(img.width);
                for (int xx = x0; xx < x1; ++xx) {
                    const float v = src[xx];
                    if (std::isfinite(v)) { sum += v; ++count; }
                }
            }
            const float v = (count > 0) ? static_cast<float>(sum / count) : 0.0f;

            float n = (v - src_min) / frange;
            n = (n - shadows) / denom;
            n = std::clamp(n, 0.0f, 1.0f);
            n = apply_mtf(n, midtone);

            const uint8_t g = to_u8(n);
            uint8_t* px = row + static_cast<ptrdiff_t>(x) * 4;
            px[0] = g;    // B
            px[1] = g;    // G
            px[2] = g;    // R
            px[3] = 255;  // A
        }
    }

    return out;
}

}  // namespace fitsx
