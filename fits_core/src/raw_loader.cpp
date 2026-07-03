#include "fits_core/raw_loader.h"

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>

namespace fitsx {

namespace {

std::string fmt_g(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return std::string(b);
}

std::string fmt_i(long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%ld", v);
    return std::string(b);
}

// time_t (camera capture, UTC) -> FITS DATE-OBS "YYYY-MM-DDThh:mm:ss".
std::string iso8601(time_t t) {
    if (t <= 0) return {};
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &t) != 0) return {};
#else
    if (!gmtime_r(&t, &tm)) return {};
#endif
    char b[32];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(b);
}

// Map LibRaw EXIF onto synthesized FITS keywords so the existing header
// display + Astro.FITS.* property plumbing surface camera RAW metadata with
// zero special-casing downstream.
void synth_headers(const LibRaw& rp, std::vector<FitsHeader>& out) {
    const libraw_iparams_t& id = rp.imgdata.idata;
    const libraw_imgother_t& ot = rp.imgdata.other;

    auto add = [&](const char* key, std::string value, const char* comment) {
        if (value.empty()) return;
        FitsHeader h;
        h.key = key;
        h.value = std::move(value);
        h.comment = comment;
        out.push_back(std::move(h));
    };

    std::string make = id.make;
    std::string model = id.model;
    std::string instr = make;
    if (!model.empty()) { if (!instr.empty()) instr += " "; instr += model; }
    add("INSTRUME", instr, "Camera make and model");

    if (ot.shutter > 0.0f)  add("EXPTIME",  fmt_g(ot.shutter),    "[s] Exposure time");
    if (ot.iso_speed > 0.0f) add("ISO",     fmt_i(static_cast<long>(ot.iso_speed + 0.5f)), "ISO speed");
    if (ot.aperture > 0.0f) add("FNUMBER",  fmt_g(ot.aperture),   "Aperture f-number");
    if (ot.focal_len > 0.0f) add("FOCALLEN", fmt_g(ot.focal_len), "[mm] Lens focal length");
    add("DATE-OBS", iso8601(ot.timestamp), "Capture time (UTC)");
}

}  // namespace

bool is_raw(const void* buffer, size_t size) noexcept {
    if (!buffer || size < 4) return false;
    const auto* b = static_cast<const uint8_t*>(buffer);
    // Little-endian TIFF "II*\0" or big-endian "MM\0*". Every TIFF-derived RAW
    // (NEF, CR2, ARW, DNG, PEF, SRW, ...) opens with one of these.
    const bool le = (b[0] == 0x49 && b[1] == 0x49 && b[2] == 0x2A && b[3] == 0x00);
    const bool be = (b[0] == 0x4D && b[1] == 0x4D && b[2] == 0x00 && b[3] == 0x2A);
    return le || be;
}

LoadResult load_raw_from_memory(const void* buffer, size_t size, bool half_res) {
    LoadResult res;
    if (!buffer || size == 0) { res.error = "Empty buffer"; return res; }

    try {
        // Heap-allocate: a LibRaw object embeds imgdata (>200 KB, dominated by
        // color.curve[65536]). On the stack it overflows the small stacks of
        // explorer.exe's shell-handler threads (observed: 0xC00000FD). The heap
        // keeps the giant struct off the caller's stack.
        auto rp = std::make_unique<LibRaw>();
        int rc = rp->open_buffer(const_cast<void*>(buffer), size);
        if (rc != LIBRAW_SUCCESS) {
            res.error = std::string("LibRaw open_buffer: ") + libraw_strerror(rc);
            return res;
        }

        // Linear 16-bit output with the camera's as-shot white balance: no
        // gamma curve, no auto-brightness, sRGB primaries. Our auto-stretch
        // does the display tone mapping, exactly as for FITS/XISF.
        libraw_output_params_t& P = rp->imgdata.params;
        P.output_bps     = 16;
        P.no_auto_bright = 1;
        P.use_camera_wb  = 1;
        P.use_auto_wb    = 0;
        P.output_color   = 1;        // sRGB
        P.gamm[0]        = 1.0;      // linear gamma
        P.gamm[1]        = 1.0;
        P.user_flip      = -1;       // honor the camera orientation flag
        // Bilinear demosaic instead of the default AHD. On a 24 Mpx frame AHD
        // spends ~4 s in dcraw_process vs ~0.7 s for bilinear (3x faster total
        // decode) at full resolution; the quality gap is invisible on-screen
        // and irrelevant for a viewer. See tools/raw_bench.cpp for the numbers.
        P.user_qual      = 0;
        // Thumbnails: demosaic at half linear resolution (quarter the pixels)
        // straight from the CFA. ~4x faster / a quarter of the RAM, and invisible
        // once the frame is downsampled to a ~256 px thumbnail.
        P.half_size      = half_res ? 1 : 0;

        // Cap the RAW-unpack allocation. This decodes attacker-supplied files
        // in-process inside explorer.exe (thumbnail / preview handlers), and
        // LibRaw has a history of decompression-bomb CVEs. 2 GB is far above any
        // real sensor's raw buffer (~120 MB for a 60 Mpx 16-bit frame) yet bounds
        // a hostile file that claims to need an absurd allocation.
        rp->imgdata.rawparams.max_raw_memory_mb = 2048;

        rc = rp->unpack();
        if (rc != LIBRAW_SUCCESS) {
            res.error = std::string("LibRaw unpack: ") + libraw_strerror(rc);
            return res;
        }
        rc = rp->dcraw_process();
        if (rc != LIBRAW_SUCCESS) {
            res.error = std::string("LibRaw process: ") + libraw_strerror(rc);
            return res;
        }

        int err = 0;
        libraw_processed_image_t* out = rp->dcraw_make_mem_image(&err);
        if (!out) {
            res.error = std::string("LibRaw make_mem_image: ") + libraw_strerror(err);
            return res;
        }
        if (out->type != LIBRAW_IMAGE_BITMAP || out->bits != 16 ||
            (out->colors != 3 && out->colors != 1)) {
            rp->dcraw_clear_mem(out);
            res.error = "Unexpected LibRaw image format";
            return res;
        }

        const int w = out->width;
        const int h = out->height;
        const int nc = out->colors;
        const size_t npix = static_cast<size_t>(w) * static_cast<size_t>(h);
        const auto* px = reinterpret_cast<const uint16_t*>(out->data);

        FitsImage& img = res.image;
        img.width  = w;
        img.height = h;
        img.source_type = PixelType::UInt16;
        img.data.resize(npix);
        const bool rgb = (nc == 3);
        if (rgb) { img.data_g.resize(npix); img.data_b.resize(npix); }

        // Copy interleaved 16-bit samples into our float planes, folding the
        // global min/max (across all channels) into the same pass. The rgb vs
        // mono choice is hoisted out of the per-pixel loop and min/max uses
        // branchless std::min/max so the hot loop (~72M samples) vectorizes.
        float vmin = std::numeric_limits<float>::infinity();
        float vmax = -std::numeric_limits<float>::infinity();
        if (rgb) {
            for (size_t i = 0; i < npix; ++i) {
                const float r = px[i * 3 + 0];
                const float g = px[i * 3 + 1];
                const float b = px[i * 3 + 2];
                img.data[i]   = r;
                img.data_g[i] = g;
                img.data_b[i] = b;
                vmin = std::min(vmin, std::min({r, g, b}));
                vmax = std::max(vmax, std::max({r, g, b}));
            }
        } else {
            for (size_t i = 0; i < npix; ++i) {
                const float v = px[i];
                img.data[i] = v;
                vmin = std::min(vmin, v);
                vmax = std::max(vmax, v);
            }
        }
        if (!std::isfinite(vmin) || vmax <= vmin) { vmin = 0.0f; vmax = 1.0f; }
        img.source_min = vmin;
        img.source_max = vmax;

        synth_headers(*rp, img.headers);

        rp->dcraw_clear_mem(out);
        rp->recycle();
        res.success = true;
        return res;
    } catch (const std::exception& e) {
        res.error = std::string("LibRaw exception: ") + e.what();
        return res;
    } catch (...) {
        res.error = "LibRaw: unknown exception";
        return res;
    }
}

RawMetadata parse_raw_metadata(const void* buffer, size_t size) {
    RawMetadata md;
    if (!buffer || size == 0) return md;
    try {
        auto rp = std::make_unique<LibRaw>();   // heap, not stack (see above)
        if (rp->open_buffer(const_cast<void*>(buffer), size) != LIBRAW_SUCCESS) return md;
        // open_buffer parses headers/EXIF without unpacking pixels.
        const libraw_image_sizes_t& s = rp->imgdata.sizes;
        md.width  = (s.iwidth  > 0) ? s.iwidth  : s.width;
        md.height = (s.iheight > 0) ? s.iheight : s.height;
        synth_headers(*rp, md.headers);
        rp->recycle();
        md.success = true;
        return md;
    } catch (...) {
        return md;
    }
}

}  // namespace fitsx
