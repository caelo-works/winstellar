// FITS export, round-tripped through the project's own loader.
//
// The strongest check available: write a file, read it back with the same code
// that opens the user's frames, and compare pixels and keywords. Most of what
// can go wrong here is invisible to the eye -- an image written 90 degrees off,
// a Bayer pattern that no longer matches its mosaic, a structural keyword
// copied over one CFITSIO wrote itself -- so none of it is left to inspection.

#include "fits_core/fits_writer.h"
#include "fits_core/fits_loader.h"
#include "fits_core/fits_image.h"
#include "fits_core/image_rotate.h"

#include "helpers/synth_fits.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using fitsx::FitsExportOptions;
using fitsx::FitsExportPath;
using fitsx::FitsExportResult;
using fitsx::FitsImage;
using fitsx::write_fits_export;

namespace {

std::wstring temp_dir_for(const wchar_t* tag) {
    wchar_t base[MAX_PATH];
    ::GetTempPathW(MAX_PATH, base);
    std::wstring d = std::wstring(base) + L"winstellar_export_" + tag + L"_" +
                     std::to_wstring(::GetCurrentProcessId());
    std::error_code ec;
    fs::create_directories(d, ec);
    return d + L"\\";
}

// A frame whose every pixel encodes its own position, plus a marker in one
// corner. Symmetry is the enemy here: a symmetric test image passes even when
// the rotation is inverted or transposed.
FitsImage make_marked(int w, int h, bool rgb = false) {
    FitsImage img;
    img.width = w;
    img.height = h;
    img.data.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.data[static_cast<size_t>(y) * w + x] =
                static_cast<float>(y * 100 + x);   // top-down, as the loader stores it
    img.data[0] = 9999.0f;                          // top-left marker
    if (rgb) {
        img.data_g = img.data;
        img.data_b = img.data;
        for (auto& v : img.data_g) v += 1.0f;
        for (auto& v : img.data_b) v += 2.0f;
    }
    return img;
}

FitsExportOptions basic_opts(int rot = 0, bool memfile = false) {
    FitsExportOptions o;
    o.display_rotation_deg = rot;
    o.source_name = "input.fits";
    o.app_version = "0.7.0-test";
    o.mode = fitsx::FitsExportMode::FromImageOnly;
    o.force_memfile_sink = memfile;
    return o;
}

fitsx::LoadResult load_back(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    return fitsx::load_from_memory(bytes.data(), bytes.size());
}

bool any_temp_left(const std::wstring& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.find(L".winstellar-tmp-") != std::wstring::npos) return true;
    }
    return false;
}

std::string header_of(const FitsImage& img, const char* key) {
    std::string v;
    img.find_header(key, v);
    return v;
}

bool has_header(const FitsImage& img, const char* key) {
    std::string v;
    return img.find_header(key, v);
}

}  // namespace

// ------------------------------------------------------------ round trip ---

class FitsExport : public ::testing::TestWithParam<bool> {};

TEST_P(FitsExport, MonoRoundTripPreservesEveryPixel) {
    const bool memfile = GetParam();
    const auto dir = temp_dir_for(L"mono");
    const auto dest = dir + L"out.fits";

    const FitsImage img = make_marked(7, 5);
    const auto res = write_fits_export(dest, img, basic_opts(0, memfile), {});
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_FALSE(any_temp_left(dir));

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;
    ASSERT_EQ(back.image.width, 7);
    ASSERT_EQ(back.image.height, 5);
    for (size_t i = 0; i < img.data.size(); ++i)
        ASSERT_FLOAT_EQ(back.image.data[i], img.data[i]) << "pixel " << i;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

INSTANTIATE_TEST_SUITE_P(BothSinks, FitsExport, ::testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& i) {
                             return i.param ? "MemfileSink" : "DiskSink";
                         });

TEST(FitsExportRotation, EveryQuarterTurnLandsWhereTheScreenShowsIt) {
    const auto dir = temp_dir_for(L"rot");
    const FitsImage img = make_marked(7, 5);

    for (int rot : {90, 180, 270}) {
        const auto dest = dir + L"rot" + std::to_wstring(rot) + L".fits";
        const auto res = write_fits_export(dest, img, basic_opts(rot), {});
        ASSERT_TRUE(res.success) << res.error;

        const auto back = load_back(dest);
        ASSERT_TRUE(back.success) << back.error;

        const fitsx::RotMap m = fitsx::make_rot_map(7, 5, rot);
        ASSERT_EQ(back.image.width, m.out_w) << "rot=" << rot;
        ASSERT_EQ(back.image.height, m.out_h) << "rot=" << rot;

        // Every destination pixel must equal its geometrically mapped source.
        for (int y = 0; y < m.out_h; ++y)
            for (int x = 0; x < m.out_w; ++x)
                ASSERT_FLOAT_EQ(back.image.data[static_cast<size_t>(y) * m.out_w + x],
                                img.data[m.src(x, y)])
                    << "rot=" << rot << " at " << x << "," << y;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportRotation, MarkerEndsUpInTheExpectedCorner) {
    // Belt and braces on the bottom-up conjugation: a 90 degree clockwise turn
    // must move the top-left marker to the top-right.
    const auto dir = temp_dir_for(L"marker");
    const auto dest = dir + L"m.fits";
    const FitsImage img = make_marked(6, 4);
    ASSERT_TRUE(write_fits_export(dest, img, basic_opts(90), {}).success);

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success);
    ASSERT_EQ(back.image.width, 4);
    ASSERT_EQ(back.image.height, 6);
    EXPECT_FLOAT_EQ(back.image.data[3], 9999.0f) << "marker is not in the top-right";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportColour, RgbCubeKeepsPlaneOrder) {
    const auto dir = temp_dir_for(L"rgb");
    const auto dest = dir + L"rgb.fits";
    const FitsImage img = make_marked(5, 3, /*rgb=*/true);

    const auto res = write_fits_export(dest, img, basic_opts(0), {});
    ASSERT_TRUE(res.success) << res.error;

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;
    ASSERT_TRUE(back.image.is_rgb());
    for (size_t i = 0; i < img.data.size(); ++i) {
        ASSERT_FLOAT_EQ(back.image.data[i],   img.data[i]);
        ASSERT_FLOAT_EQ(back.image.data_g[i], img.data_g[i]);
        ASSERT_FLOAT_EQ(back.image.data_b[i], img.data_b[i]);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// --------------------------------------------------------- header policy ---

TEST(FitsExportHeaders, StructuralKeywordsAreNotCopiedOverCfitsios) {
    const auto dir = temp_dir_for(L"hdr");
    const auto dest = dir + L"h.fits";

    FitsImage img = make_marked(6, 4);
    // A source whose keyword list carries structural cards, as an XISF's
    // FITSKeyword list can. Copying these would contradict the array written.
    img.headers = {
        {"SIMPLE",  "T",     ""},
        {"BITPIX",  "16",    ""},
        {"NAXIS",   "2",     ""},
        {"NAXIS1",  "9999",  ""},
        {"NAXIS2",  "9999",  ""},
        {"BZERO",   "32768", ""},
        {"BSCALE",  "1",     ""},
        {"OBJECT",  "7331",  "target"},
        {"EXPTIME", "300.0", "[s]"},
        {"GAIN",    "120",   ""},
        {"FOCUSED", "T",     ""},
        {"COMMENT", "",      "a free-form remark"},
        {"HISTORY", "",      "stacked by something"},
    };

    ASSERT_TRUE(write_fits_export(dest, img, basic_opts(0), {}).success);
    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;

    // Dimensions come from the array, not from the copied lie.
    EXPECT_EQ(back.image.width, 6);
    EXPECT_EQ(back.image.height, 4);
    EXPECT_EQ(header_of(back.image, "NAXIS1"), "6");
    EXPECT_EQ(header_of(back.image, "BITPIX"), "-32");

    // OBJECT = 7331 is NGC 7331, and must not become the integer 7331.
    EXPECT_EQ(header_of(back.image, "OBJECT"), "7331");
    EXPECT_EQ(header_of(back.image, "GAIN"), "120");
    EXPECT_NEAR(std::stod(header_of(back.image, "EXPTIME")), 300.0, 1e-9);
    EXPECT_EQ(header_of(back.image, "FOCUSED"), "T");

    // Our own provenance.
    EXPECT_NE(header_of(back.image, "PROGRAM").find("WinStellar"), std::string::npos);
    EXPECT_EQ(header_of(back.image, "ROWORDER"), "BOTTOM-UP");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportHeaders, RgbOutputCarriesNoBayerKeywords) {
    const auto dir = temp_dir_for(L"rgbcfa");
    const auto dest = dir + L"c.fits";

    FitsImage img = make_marked(6, 4, /*rgb=*/true);
    img.headers = { {"BAYERPAT", "RGGB", ""}, {"XBAYROFF", "0", ""} };

    ASSERT_TRUE(write_fits_export(dest, img, basic_opts(0), {}).success);
    const auto back = load_back(dest);
    ASSERT_TRUE(back.success);
    // Leaving BAYERPAT on already-demosaiced data makes the next tool demosaic
    // it a second time.
    EXPECT_FALSE(has_header(back.image, "BAYERPAT"));
    EXPECT_FALSE(has_header(back.image, "XBAYROFF"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportHeaders, RotatingAMosaicRewritesItsBayerPattern) {
    const auto dir = temp_dir_for(L"cfarot");
    FitsImage img = make_marked(6, 4);
    img.headers = { {"BAYERPAT", "RGGB", ""} };

    struct { int rot; const char* want; } cases[] = {
        {0,   "RGGB"}, {90,  "GBRG"}, {180, "BGGR"}, {270, "GRBG"},
    };
    for (const auto& c : cases) {
        const auto dest = dir + L"b" + std::to_wstring(c.rot) + L".fits";
        const auto res = write_fits_export(dest, img, basic_opts(c.rot), {});
        ASSERT_TRUE(res.success) << res.error;
        EXPECT_TRUE(res.carries_cfa);

        const auto back = load_back(dest);
        ASSERT_TRUE(back.success);
        EXPECT_EQ(header_of(back.image, "BAYERPAT"), c.want)
            << "rot=" << c.rot << ": a mosaic written with the wrong pattern "
               "demosaics with swapped colours downstream";
        EXPECT_EQ(header_of(back.image, "XBAYROFF"), "0");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportHeaders, RotationDropsOrientationDependentWcs) {
    const auto dir = temp_dir_for(L"wcs");
    FitsImage img = make_marked(6, 4);
    img.headers = {
        {"CTYPE1", "RA---TAN", ""}, {"CRPIX1", "512.0", ""},
        {"CD1_1",  "-0.0002",  ""}, {"A_2_0",  "1e-6",  ""},
        {"EQUINOX", "2000.0",  ""}, {"OBJCTRA", "00 42 44", ""},
    };

    {   // Unrotated: astrometry is still valid, so it stays.
        const auto dest = dir + L"w0.fits";
        ASSERT_TRUE(write_fits_export(dest, img, basic_opts(0), {}).success);
        const auto back = load_back(dest);
        ASSERT_TRUE(back.success);
        EXPECT_TRUE(has_header(back.image, "CTYPE1"));
        EXPECT_TRUE(has_header(back.image, "CD1_1"));
    }
    {   // Rotated: a stale solution is worse than none.
        const auto dest = dir + L"w90.fits";
        ASSERT_TRUE(write_fits_export(dest, img, basic_opts(90), {}).success);
        const auto back = load_back(dest);
        ASSERT_TRUE(back.success);
        EXPECT_FALSE(has_header(back.image, "CTYPE1"));
        EXPECT_FALSE(has_header(back.image, "CRPIX1"));
        EXPECT_FALSE(has_header(back.image, "CD1_1"));
        EXPECT_FALSE(has_header(back.image, "A_2_0"));
        // Frame-of-reference values survive a rotation.
        EXPECT_TRUE(has_header(back.image, "EQUINOX"));
        EXPECT_TRUE(has_header(back.image, "OBJCTRA"));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------------------- integrity ---

TEST(FitsExportIntegrity, CancellationLeavesNothingBehind) {
    const auto dir = temp_dir_for(L"cancel");
    const auto dest = dir + L"c.fits";
    const FitsImage img = make_marked(64, 64, /*rgb=*/true);

    const auto res = write_fits_export(dest, img, basic_opts(0), [] { return true; });
    EXPECT_FALSE(res.success);
    EXPECT_TRUE(res.cancelled);
    EXPECT_FALSE(fs::exists(dest));
    EXPECT_FALSE(any_temp_left(dir));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportIntegrity, AFailedExportDoesNotDamageAnExistingFile) {
    const auto dir = temp_dir_for(L"keep");
    const auto dest = dir + L"existing.fits";
    {
        std::ofstream f(dest, std::ios::binary);
        f << "PRECIOUS";
    }
    const FitsImage img = make_marked(32, 32);
    const auto res = write_fits_export(dest, img, basic_opts(0), [] { return true; });
    ASSERT_FALSE(res.success);

    std::ifstream f(dest, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "PRECIOUS") << "the destination was clobbered before the write succeeded";
    EXPECT_FALSE(any_temp_left(dir));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportIntegrity, OverwriteReplacesTheFileWholesale) {
    const auto dir = temp_dir_for(L"over");
    const auto dest = dir + L"o.fits";
    {
        std::ofstream f(dest, std::ios::binary);
        f << std::string(100000, 'x');   // deliberately longer than the export
    }
    const FitsImage img = make_marked(8, 8);
    ASSERT_TRUE(write_fits_export(dest, img, basic_opts(0), {}).success);

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << "a replaced file must not keep the old tail";
    EXPECT_EQ(back.image.width, 8);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportIntegrity, MemfileLengthIsTheFileNotTheAllocation) {
    // The memfile sink grows in 16 MB steps; writing the allocation instead of
    // the file length appends uninitialised heap that some readers accept and
    // others reject.
    const auto dir = temp_dir_for(L"memlen");
    const auto dest = dir + L"m.fits";
    const FitsImage img = make_marked(64, 64);
    ASSERT_TRUE(write_fits_export(dest, img, basic_opts(0, /*memfile=*/true), {}).success);

    const auto size = fs::file_size(dest);
    EXPECT_EQ(size % 2880, 0u) << "a FITS file is a whole number of 2880-byte blocks";
    EXPECT_LT(size, 1u << 20) << "the allocated buffer was written instead of the file";

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportIntegrity, UnicodeDestinationIsHandled) {
    // A path the ANSI code page may not express must still export -- via the
    // memfile sink and a wide-path write.
    wchar_t base[MAX_PATH];
    ::GetTempPathW(MAX_PATH, base);
    const std::wstring dir = std::wstring(base) + L"winstellar_\u00c9t\u00e9_" +
                             std::to_wstring(::GetCurrentProcessId()) + L"\\";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto dest = dir + L"\u661f\u7a7a.fits";

    const FitsImage img = make_marked(8, 6);
    const auto res = write_fits_export(dest, img, basic_opts(0), {});
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_TRUE(fs::exists(dest));
    EXPECT_FALSE(any_temp_left(dir));

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;
    EXPECT_EQ(back.image.width, 8);

    fs::remove_all(dir, ec);
}

// ----------------------------------------------------------------- probe ---

TEST(FitsSourceProbe, ReportsWhyASourceCannotBeReused) {
    EXPECT_EQ(fitsx::probe_fits_source(L"", 0, 0), fitsx::FitsSourceStatus::NoPath);
    EXPECT_EQ(fitsx::probe_fits_source(L"Z:\\nope\\gone.fits", 0, 0),
              fitsx::FitsSourceStatus::Missing);

    const auto dir = temp_dir_for(L"probe");
    const auto notfits = dir + L"x.fits";
    { std::ofstream f(notfits, std::ios::binary); f << "not a fits file at all"; }
    EXPECT_EQ(fitsx::probe_fits_source(notfits, 0, 0), fitsx::FitsSourceStatus::NotFits);

    // A real FITS we just wrote is usable, and the dimension guard notices when
    // the file on disk stops matching what is on screen.
    const auto good = dir + L"g.fits";
    const FitsImage img = make_marked(9, 6);
    ASSERT_TRUE(write_fits_export(good, img, basic_opts(0), {}).success);
    EXPECT_EQ(fitsx::probe_fits_source(good, 9, 6), fitsx::FitsSourceStatus::Usable);
    EXPECT_EQ(fitsx::probe_fits_source(good, 9, 7), fitsx::FitsSourceStatus::Changed);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------- export from the source ---
//
// The owner's rule is that an exported FITS carries the ORIGINAL CFA mosaic.
// The in-memory image cannot supply it -- the loader frees the CFA buffer right
// after demosaicing -- so these cover the branch that re-reads the source file.

namespace {

std::string narrow_path(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::vector<uint8_t> read_all(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

}  // namespace

TEST(FitsExportFromSource, UnrotatedExportIsAByteForByteCopy) {
    const auto dir = temp_dir_for(L"srccopy");
    const auto src = dir + L"src.fits";
    std::vector<uint16_t> px(6 * 4);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint16_t>(i * 1000);
    ASSERT_FALSE(wst::write_synth_fits_u16(narrow_path(src), 6, 4, px,
                                           {{"OBJECT", "M31"}, {"BAYERPAT", "RGGB"}},
                                           {{"EXPTIME", 300.0}}).empty());

    FitsExportOptions o = basic_opts(0);
    o.mode = fitsx::FitsExportMode::Auto;
    o.source_path = src;
    o.expect_width = 6;
    o.expect_height = 4;

    const auto dest = dir + L"out.fits";
    const auto res = write_fits_export(dest, FitsImage{}, o, {});
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.path, FitsExportPath::VerbatimCopy);
    EXPECT_TRUE(res.carries_cfa);
    EXPECT_EQ(read_all(src), read_all(dest)) << "an unrotated export must be the original bytes";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportFromSource, RotatedExportKeepsTheMosaicBitDepthAndKeywords) {
    const auto dir = temp_dir_for(L"srcrot");
    const auto src = dir + L"src.fits";

    // Each pixel encodes its own CFA colour, so a mosaic that moves without its
    // pattern being rewritten is detectable.
    const int W = 6, H = 4;
    std::vector<uint16_t> px(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            px[static_cast<size_t>(y) * W + x] =
                static_cast<uint16_t>(1000 + y * 10 + x);
    ASSERT_FALSE(wst::write_synth_fits_u16(narrow_path(src), W, H, px,
                                           {{"OBJECT", "M31"}, {"BAYERPAT", "RGGB"},
                                            {"IMAGETYP", "LIGHT"}},
                                           {{"EXPTIME", 300.0}, {"GAIN", 120.0}}).empty());

    FitsExportOptions o = basic_opts(90);
    o.mode = fitsx::FitsExportMode::Auto;
    o.source_path = src;
    o.expect_width = W;
    o.expect_height = H;

    const auto dest = dir + L"out.fits";
    const auto res = write_fits_export(dest, FitsImage{}, o, {});
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.path, FitsExportPath::SourceReencode);
    EXPECT_TRUE(res.carries_cfa);

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success) << back.error;
    EXPECT_EQ(back.image.width, H);      // dimensions swap on a quarter turn
    EXPECT_EQ(back.image.height, W);

    // Native bit depth survives: still BITPIX=16 with its BZERO, not float.
    EXPECT_EQ(header_of(back.image, "BITPIX"), "16");
    EXPECT_EQ(header_of(back.image, "BZERO"), "32768");

    // Keywords survive with their types.
    EXPECT_EQ(header_of(back.image, "OBJECT"), "M31");
    EXPECT_EQ(header_of(back.image, "IMAGETYP"), "LIGHT");
    EXPECT_NEAR(std::stod(header_of(back.image, "EXPTIME")), 300.0, 1e-9);

    // And the pattern followed its mosaic.
    EXPECT_EQ(header_of(back.image, "BAYERPAT"), "GBRG");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportFromSource, PixelsSurviveTheRotationExactly) {
    const auto dir = temp_dir_for(L"srcpix");
    const auto src = dir + L"src.fits";
    const int W = 5, H = 3;
    std::vector<uint16_t> px(static_cast<size_t>(W) * H);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint16_t>(7 + i * 13);
    ASSERT_FALSE(wst::write_synth_fits_u16(narrow_path(src), W, H, px, {}, {}).empty());

    // What the viewer shows for that file, so the expected result can be derived
    // independently of the writer.
    const auto shown = load_back(src);
    ASSERT_TRUE(shown.success);

    FitsExportOptions o = basic_opts(270);
    o.mode = fitsx::FitsExportMode::Auto;
    o.source_path = src;
    o.expect_width = W;
    o.expect_height = H;

    const auto dest = dir + L"out.fits";
    ASSERT_TRUE(write_fits_export(dest, FitsImage{}, o, {}).success);

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success);
    const fitsx::RotMap m = fitsx::make_rot_map(W, H, 270);
    ASSERT_EQ(back.image.width, m.out_w);
    ASSERT_EQ(back.image.height, m.out_h);
    for (int y = 0; y < m.out_h; ++y)
        for (int x = 0; x < m.out_w; ++x)
            ASSERT_FLOAT_EQ(back.image.data[static_cast<size_t>(y) * m.out_w + x],
                            shown.image.data[m.src(x, y)])
                << "at " << x << "," << y;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportFromSource, AChangedSourceFallsBackToTheImage) {
    const auto dir = temp_dir_for(L"srcchg");
    const auto src = dir + L"src.fits";
    std::vector<uint16_t> px(6 * 4, 100);
    ASSERT_FALSE(wst::write_synth_fits_u16(narrow_path(src), 6, 4, px, {}, {}).empty());

    FitsExportOptions o = basic_opts(0);
    o.mode = fitsx::FitsExportMode::Auto;
    o.source_path = src;
    o.expect_width = 9;      // no longer what is on disk
    o.expect_height = 9;

    const FitsImage img = make_marked(9, 9);
    const auto dest = dir + L"out.fits";
    const auto res = write_fits_export(dest, img, o, {});
    ASSERT_TRUE(res.success) << res.error;
    EXPECT_EQ(res.path, FitsExportPath::FromImage)
        << "a source that no longer matches the screen must not be copied";

    const auto back = load_back(dest);
    ASSERT_TRUE(back.success);
    EXPECT_EQ(back.image.width, 9);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportFromSource, MissingSourceFallsBackAndFromSourceOnlyRefuses) {
    const auto dir = temp_dir_for(L"srcgone");
    FitsExportOptions o = basic_opts(0);
    o.source_path = dir + L"never-existed.fits";
    o.expect_width = 8;
    o.expect_height = 6;

    const FitsImage img = make_marked(8, 6);

    o.mode = fitsx::FitsExportMode::Auto;
    const auto dest = dir + L"a.fits";
    const auto fallback = write_fits_export(dest, img, o, {});
    ASSERT_TRUE(fallback.success) << fallback.error;
    EXPECT_EQ(fallback.path, FitsExportPath::FromImage);
    EXPECT_FALSE(fallback.carries_cfa);

    // The strict mode is what the UI uses once the user has declined the
    // fallback: it must not quietly write something else.
    o.mode = fitsx::FitsExportMode::FromSourceOnly;
    const auto strict = write_fits_export(dir + L"b.fits", img, o, {});
    EXPECT_FALSE(strict.success);
    EXPECT_FALSE(strict.error.empty());
    EXPECT_FALSE(fs::exists(dir + L"b.fits"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(FitsExportFromSource, UnknownBayerPatternRefusesToRotate) {
    const auto dir = temp_dir_for(L"srcxtrans");
    const auto src = dir + L"src.fits";
    std::vector<uint16_t> px(6 * 4, 42);
    ASSERT_FALSE(wst::write_synth_fits_u16(narrow_path(src), 6, 4, px,
                                           {{"BAYERPAT", "XTRANS"}}, {}).empty());

    // Rotating a mosaic whose pattern we cannot name would ship a file that
    // decodes with swapped colours, so the probe refuses.
    EXPECT_EQ(fitsx::probe_fits_source(src, 6, 4, 90),
              fitsx::FitsSourceStatus::UnrotatableCfa);
    EXPECT_EQ(fitsx::probe_fits_source(src, 6, 4, 0),
              fitsx::FitsSourceStatus::Usable) << "unrotated is a byte copy; nothing can go wrong";

    std::error_code ec;
    fs::remove_all(dir, ec);
}
