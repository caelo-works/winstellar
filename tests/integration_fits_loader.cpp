#include "fits_core/fits_loader.h"
#include "helpers/synth_fits.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <filesystem>
#include <string>

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
