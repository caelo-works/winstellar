#include "fits_core/fits_loader.h"
#include "helpers/synth_fits.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Per-test temp file in %TEMP%, deleted on TearDown. Avoids ordering issues
// with concurrent tests running from CTest.
class TempFitsFile {
public:
    explicit TempFitsFile(const std::string& tag) {
        wchar_t base[MAX_PATH];
        ::GetTempPathW(MAX_PATH, base);
        path_ = fs::path(base) / (L"winstellar_test_" + std::wstring(tag.begin(), tag.end()) + L".fits");
        fs::remove(path_);
    }
    ~TempFitsFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    std::string  utf8 () const { return path_.string(); }
    std::wstring utf16() const { return path_.wstring(); }
private:
    fs::path path_;
};

}  // namespace

TEST(FitsLoader, LoadsSyntheticConstantImage) {
    TempFitsFile f("constant");
    auto spec = wst::make_constant(32, 24, 1234.0f);
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());

    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.image.width,  32);
    EXPECT_EQ(r.image.height, 24);
    EXPECT_EQ(r.image.data.size(), 32u * 24u);
    // Every pixel should be the constant we wrote.
    for (float v : r.image.data) EXPECT_FLOAT_EQ(v, 1234.0f);
    // BSCALE/BZERO default to 1/0 when not present.
    EXPECT_DOUBLE_EQ(r.image.bzero,  0.0);
    EXPECT_DOUBLE_EQ(r.image.bscale, 1.0);
    // Note: for a constant image vmin == vmax, so the loader applies its
    // [0,1] fallback to source_min/max to avoid downstream zero-division —
    // this is intentional, not a bug.
    EXPECT_DOUBLE_EQ(r.image.source_min, 0.0);
    EXPECT_DOUBLE_EQ(r.image.source_max, 1.0);
}

TEST(FitsLoader, RecordsSourceRangeFromVaryingPixels) {
    TempFitsFile f("ramp");
    wst::SynthFits spec;
    spec.width = 16; spec.height = 16;
    spec.pixels.resize(256);
    for (size_t i = 0; i < spec.pixels.size(); ++i)
        spec.pixels[i] = 100.0f + static_cast<float>(i);   // 100..355
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());

    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_DOUBLE_EQ(r.image.source_min, 100.0);
    EXPECT_DOUBLE_EQ(r.image.source_max, 355.0);
}

TEST(FitsLoader, PreservesUserKeywords) {
    TempFitsFile f("keywords");
    auto spec = wst::make_constant(16, 16, 500.0f);
    spec.extra_keywords = {
        {"OBJECT",  "M33"},
        {"FILTER",  "Ha"},
    };
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());

    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;

    auto find = [&](const std::string& key) -> std::string {
        for (const auto& h : r.image.headers) if (h.key == key) return h.value;
        return {};
    };
    EXPECT_EQ(find("OBJECT"), "M33");
    EXPECT_EQ(find("FILTER"), "Ha");
}

TEST(FitsLoader, FlipsRowOrderToTopDown) {
    // Write an image whose first FITS row (bottom of image) is 0 and last
    // row (top of image) is 100. After load we expect row 0 (top of our
    // BGRA-ordered buffer) to hold the 100 value.
    TempFitsFile f("flip");
    wst::SynthFits spec;
    spec.width = 4;
    spec.height = 4;
    spec.pixels.assign(16, 0.0f);
    // Last row in FITS-order (top of image) = 100.
    for (int x = 0; x < 4; ++x) spec.pixels[12 + x] = 100.0f;
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());

    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    // Top row in the loaded image (data[0..3]) should now hold 100.
    EXPECT_FLOAT_EQ(r.image.data[0], 100.0f);
    EXPECT_FLOAT_EQ(r.image.data[1], 100.0f);
    EXPECT_FLOAT_EQ(r.image.data[2], 100.0f);
    EXPECT_FLOAT_EQ(r.image.data[3], 100.0f);
    EXPECT_FLOAT_EQ(r.image.data[12], 0.0f);   // bottom row = 0
}

TEST(FitsLoader, MonoFrameStaysMono) {
    // A plain 2-D frame with no BAYERPAT must not be treated as color.
    TempFitsFile f("mono");
    auto spec = wst::make_constant(16, 16, 800.0f);
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());
    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_FALSE(r.image.is_rgb());
    EXPECT_TRUE(r.image.data_g.empty());
}

TEST(FitsLoader, DebayersRggbAndKeepsRedInRedChannel) {
    // Neutral background (all sites = 1000 -> R=G=B after demosaic, so
    // gray-world is a near no-op) with a central red blob: only the Red
    // photosites are boosted. After debayer the blob must dominate the RED
    // channel -- if the row-flip swapped the Bayer parity, it would land in
    // Blue instead, which this test rejects.
    const int w = 32, h = 32;
    auto color_rggb = [](int x, int y) {
        static const int t[2][2] = { {0, 1}, {1, 2} };
        return t[y & 1][x & 1];
    };
    wst::SynthFits spec;
    spec.width = w; spec.height = h;
    spec.pixels.assign(static_cast<size_t>(w) * h, 1000.0f);
    for (int y = 12; y < 20; ++y)
        for (int x = 12; x < 20; ++x)
            if (color_rggb(x, y) == 0)   // Red photosites in the blob
                spec.pixels[static_cast<size_t>(y) * w + x] = 5000.0f;
    spec.extra_keywords = { {"BAYERPAT", "RGGB"} };

    TempFitsFile f("bayer");
    ASSERT_FALSE(wst::write_synth_fits(f.utf8(), spec).empty());
    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_TRUE(r.image.is_rgb());
    EXPECT_EQ(r.image.data.size(),   static_cast<size_t>(w) * h);
    EXPECT_EQ(r.image.data_g.size(), static_cast<size_t>(w) * h);
    EXPECT_EQ(r.image.data_b.size(), static_cast<size_t>(w) * h);

    // Brightest Red pixel is the blob; it must out-shine its own G/B there.
    size_t mi = 0;
    for (size_t i = 1; i < r.image.data.size(); ++i)
        if (r.image.data[i] > r.image.data[mi]) mi = i;
    EXPECT_GT(r.image.data[mi], 3000.0f);
    EXPECT_GT(r.image.data[mi], r.image.data_g[mi] * 1.8f);
    EXPECT_GT(r.image.data[mi], r.image.data_b[mi] * 1.8f);

    // The blue channel never received the blob (no R/B swap).
    float maxb = 0.0f;
    for (float v : r.image.data_b) maxb = std::max(maxb, v);
    EXPECT_LT(maxb, 2500.0f);
}

TEST(FitsLoader, LoadsThreePlaneRgbCube) {
    const int w = 16, h = 16;
    const size_t npix = static_cast<size_t>(w) * h;
    std::vector<float> rr(npix, 1000.0f), gg(npix, 2000.0f), bb(npix, 3000.0f);
    TempFitsFile f("cube");
    ASSERT_FALSE(wst::write_synth_fits_rgb_cube(f.utf8(), w, h, rr, gg, bb).empty());

    auto r = fitsx::load_from_file(f.utf16().c_str());
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_TRUE(r.image.is_rgb());
    EXPECT_EQ(r.image.width, w);
    EXPECT_EQ(r.image.height, h);
    // Cube path applies no white balance: constants survive verbatim.
    EXPECT_FLOAT_EQ(r.image.data[0],   1000.0f);
    EXPECT_FLOAT_EQ(r.image.data_g[0], 2000.0f);
    EXPECT_FLOAT_EQ(r.image.data_b[0], 3000.0f);
}

TEST(FitsLoader, ReportsErrorOnMissingFile) {
    auto r = fitsx::load_from_file(L"C:\\path\\that\\does\\not\\exist\\nope.fits");
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.error.empty());
}

TEST(FitsLoader, ReportsErrorOnNonFitsContent) {
    // Write garbage bytes to a .fits path — load_from_memory should refuse.
    TempFitsFile f("notfits");
    {
        FILE* fp = nullptr;
        _wfopen_s(&fp, f.utf16().c_str(), L"wb");
        ASSERT_NE(fp, nullptr);
        const char garbage[] = "this is definitely not a FITS file";
        fwrite(garbage, 1, sizeof(garbage) - 1, fp);
        fclose(fp);
    }
    auto r = fitsx::load_from_file(f.utf16().c_str());
    EXPECT_FALSE(r.success);
}
