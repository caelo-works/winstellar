// Standalone benchmark for the LibRaw camera-RAW decode pipeline.
// Usage: raw_bench.exe <file.nef> [...]
//
// For each file it times the LibRaw stages (open_buffer / unpack /
// dcraw_process / make_mem_image) under three demosaic configurations so we
// can pick the right speed/quality trade-off for the viewer:
//   - AHD  (user_qual=3): LibRaw default, highest quality, slowest
//   - LIN  (user_qual=0): bilinear, full resolution, faster demosaic
//   - HALF (half_size=1): superpixel 2x2 -> 1, quarter the pixels, fastest

#include <libraw/libraw.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

static void bench(const char* name, const std::vector<uint8_t>& buf,
                  int user_qual, int half_size) {
    LibRaw rp;
    if (rp.open_buffer(const_cast<uint8_t*>(buf.data()), buf.size()) != LIBRAW_SUCCESS) {
        std::printf("  %-5s open failed\n", name);
        return;
    }
    auto& P = rp.imgdata.params;
    P.output_bps     = 16;
    P.no_auto_bright = 1;
    P.use_camera_wb  = 1;
    P.output_color   = 1;
    P.gamm[0] = P.gamm[1] = 1.0;
    P.user_qual = user_qual;
    P.half_size = half_size;

    auto t1 = clk::now(); rp.unpack();         const double t_unpack  = ms_since(t1);
    auto t2 = clk::now(); rp.dcraw_process();  const double t_process = ms_since(t2);
    int err = 0;
    auto t3 = clk::now();
    libraw_processed_image_t* img = rp.dcraw_make_mem_image(&err);
    const double t_make = ms_since(t3);

    const int w = img ? img->width : 0;
    const int h = img ? img->height : 0;
    std::printf("  %-5s %5dx%-5d  unpack %6.1f  process %7.1f  make %6.1f  | total %7.1f ms\n",
                name, w, h, t_unpack, t_process, t_make,
                t_unpack + t_process + t_make);
    if (img) rp.dcraw_clear_mem(img);
    rp.recycle();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wprintf(L"Usage: raw_bench.exe <file.nef> [...]\n");
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

        bench("AHD",  buf, 3, 0);
        bench("LIN",  buf, 0, 0);
        bench("HALF", buf, 0, 1);
    }
    return 0;
}
