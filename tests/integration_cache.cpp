#include "fits_core/cache.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// Point the singleton AnalysisCache at a scratch DB before any test touches
// it. The override is read once by ensure_open(); the test process must set
// it before the first lookup/store call. Environment is shared per process,
// so this also keeps the user's real cache clean.
class CacheEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        wchar_t base[MAX_PATH];
        ::GetTempPathW(MAX_PATH, base);
        path_ = fs::path(base) / "winstellar_test_cache.db";
        std::error_code ec;
        fs::remove(path_, ec);
        _wputenv_s(L"WINSTELLAR_CACHE_DB", path_.wstring().c_str());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove(path_, ec);
        _wputenv_s(L"WINSTELLAR_CACHE_DB", L"");
    }
private:
    fs::path path_;
};

// Registers itself with the gtest framework at static-init time so gtest_main
// will SetUp / TearDown the override around every test in this binary.
auto* const g_env = ::testing::AddGlobalTestEnvironment(new CacheEnvironment);

}  // namespace

TEST(AnalysisCache, MissBeforeStore) {
    auto& c = fitsx::AnalysisCache::instance();
    auto r = c.lookup("0000000000000001");
    EXPECT_FALSE(r.has_value());
}

TEST(AnalysisCache, RoundTripsAllFields) {
    fitsx::AnalysisResult original{};
    original.success = true;
    original.star_count = 42;
    original.hfr_median = 2.345;
    original.hfr_stddev = 0.456;
    original.fwhm_median = 5.6;
    original.eccentricity_median = 0.12;
    original.mean = 1234.5;
    original.stddev = 67.8;
    original.median = 1200.0;
    original.mad = 50.0;
    original.min_value = 100.0;
    original.min_count = 7;
    original.max_value = 65535.0;
    original.max_count = 3;

    auto& c = fitsx::AnalysisCache::instance();
    const std::string_view key = "0123456789abcdef";
    c.store(key, original);

    auto got = c.lookup(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->success,              original.success);
    EXPECT_EQ(got->star_count,           original.star_count);
    EXPECT_DOUBLE_EQ(got->hfr_median,    original.hfr_median);
    EXPECT_DOUBLE_EQ(got->hfr_stddev,    original.hfr_stddev);
    EXPECT_DOUBLE_EQ(got->fwhm_median,   original.fwhm_median);
    EXPECT_DOUBLE_EQ(got->eccentricity_median, original.eccentricity_median);
    EXPECT_DOUBLE_EQ(got->mean,          original.mean);
    EXPECT_DOUBLE_EQ(got->stddev,        original.stddev);
    EXPECT_DOUBLE_EQ(got->median,        original.median);
    EXPECT_DOUBLE_EQ(got->mad,           original.mad);
    EXPECT_DOUBLE_EQ(got->min_value,     original.min_value);
    EXPECT_EQ(got->min_count,            original.min_count);
    EXPECT_DOUBLE_EQ(got->max_value,     original.max_value);
    EXPECT_EQ(got->max_count,            original.max_count);
}

TEST(AnalysisCache, StoreOverwritesPrior) {
    fitsx::AnalysisResult v1{}; v1.success = true; v1.star_count = 10;
    fitsx::AnalysisResult v2{}; v2.success = true; v2.star_count = 99;
    const std::string_view key = "fedcba9876543210";

    auto& c = fitsx::AnalysisCache::instance();
    c.store(key, v1);
    c.store(key, v2);
    auto got = c.lookup(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->star_count, 99);
}
