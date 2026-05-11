// Standalone benchmark for the XISF header parser + full loader.
// Usage: xisf_bench.exe <file.xisf> [...]
//
// For each file: prints header parse time + full load time + image stats.

#include "fits_core/xisf_loader.h"
#include "fits_core/fits_image.h"
#include "fits_core/fits_stretch.h"
#include "fits_core/fits_render.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wprintf(L"Usage: xisf_bench.exe <file.xisf> [...]\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        std::wprintf(L"\n=== %s ===\n", argv[i]);

        auto t_io = clk::now();
        std::ifstream f(argv[i], std::ios::binary | std::ios::ate);
        if (!f) { std::wprintf(L"  open failed\n"); continue; }
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(buf.data()), sz);
        f.close();
        std::wprintf(L"  read %lld bytes in %.1f ms\n",
                     static_cast<long long>(sz), ms_since(t_io));

        // Header parse only (using just the first 32 KB; if the XML is huge we will
        // re-run with the full buffer below).
        const size_t probe = std::min<size_t>(static_cast<size_t>(sz), 32 * 1024);
        auto t_h = clk::now();
        auto hp = fitsx::parse_xisf_header(buf.data(), probe);
        const double hdr_ms = ms_since(t_h);
        if (!hp.success && probe < static_cast<size_t>(sz)) {
            // XML extends beyond 32 KB — retry with whatever we have.
            t_h = clk::now();
            hp = fitsx::parse_xisf_header(buf.data(), buf.size());
            std::wprintf(L"  header (full buffer) parsed in %.1f ms (XML=%u bytes)\n",
                         ms_since(t_h), hp.xml_length);
        } else {
            std::wprintf(L"  header (32KB probe) parsed in %.1f ms (XML=%u bytes)\n",
                         hdr_ms, hp.xml_length);
        }
        if (!hp.success) {
            std::wprintf(L"  ERROR: %hs\n", hp.error.c_str());
            continue;
        }
        const auto& h = hp.header;
        std::wprintf(L"    geometry=%dx%dx%d sampleFormat=%d colorRGB=%d\n",
                     h.width, h.height, h.channels, (int)h.sample_format,
                     (int)h.color_rgb);
        std::wprintf(L"    pixel_offset=%llu pixel_size=%llu compressed=%d\n",
                     (unsigned long long)h.pixel_offset,
                     (unsigned long long)h.pixel_size,
                     (int)h.compressed);
        std::wprintf(L"    FITS keywords: %zu\n", h.fits_keywords.size());

        // Full load
        auto t_l = clk::now();
        auto lr = fitsx::load_xisf_from_memory(buf.data(), buf.size());
        std::wprintf(L"  full load (channel 0 only) in %.1f ms : %s\n",
                     ms_since(t_l),
                     lr.success ? L"OK" : L"FAIL");
        if (!lr.success) {
            std::wprintf(L"    ERROR: %hs\n", lr.error.c_str());
            continue;
        }
        std::wprintf(L"    image: %dx%d  min=%.3f max=%.3f  headers=%zu\n",
                     lr.image.width, lr.image.height,
                     lr.image.source_min, lr.image.source_max,
                     lr.image.headers.size());

        // Mimic ThumbnailProvider pipeline: auto-stretch + render to BGRA at
        // a target thumbnail size (256), then a preview-pane size (1024).
        for (int target : {256, 1024}) {
            auto t_s = clk::now();
            auto stretch = fitsx::compute_auto_stretch(lr.image);
            const double sms = ms_since(t_s);
            auto t_r = clk::now();
            auto bmp = fitsx::render_to_bgra(lr.image, stretch, target, target);
            const double rms = ms_since(t_r);
            std::wprintf(L"    target=%d -> stretch %.1f ms + render %.1f ms = %.1f ms (out %dx%d)\n",
                         target, sms, rms, sms + rms, bmp.width, bmp.height);
        }
    }
    return 0;
}
