#include "fits_core/cache.h"
#include "fits_core/analysis.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

// --- compute_cache_key (128-bit, prefix+size) ------------------------------

TEST(CacheKey, IsDeterministicAnd32Hex) {
    std::vector<uint8_t> a(1000);
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<uint8_t>(i * 31 + 7);
    const std::string k1 = fitsx::compute_cache_key(a.data(), a.size());
    const std::string k2 = fitsx::compute_cache_key(a.data(), a.size());
    EXPECT_EQ(k1, k2);
    EXPECT_EQ(k1.size(), 32u);
    EXPECT_EQ(k1.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(CacheKey, MatchesAcrossCallersSharingPrefixAndSize) {
    // The property handler passes a large buffer + true size; the viewer's
    // from-file path passes only the sampled prefix + true size. Both must
    // derive the SAME key so the viewer-populated cache is readable in Explorer.
    const size_t file_size = 5'000'000;                 // pretend the file is 5 MB
    std::vector<uint8_t> big(200 * 1024);               // handler view (>64 KB)
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 13);
    std::vector<uint8_t> head(64 * 1024);               // from-file view (prefix)
    std::copy(big.begin(), big.begin() + head.size(), head.begin());

    const std::string k_handler  = fitsx::compute_cache_key(big.data(),  file_size);
    const std::string k_fromfile = fitsx::compute_cache_key(head.data(), file_size);
    EXPECT_EQ(k_handler, k_fromfile);
}

TEST(CacheKey, ChangesWithSizeAndContent) {
    std::vector<uint8_t> a(2000, 0x11);
    const std::string base = fitsx::compute_cache_key(a.data(), 2000);

    // Same bytes, different declared size.
    EXPECT_NE(base, fitsx::compute_cache_key(a.data(), 2001));

    // Same size, one byte flipped within the sampled prefix.
    a[123] = 0x12;
    EXPECT_NE(base, fitsx::compute_cache_key(a.data(), 2000));
}

TEST(MetadataCache, MissBeforeStore) {
    auto& c = fitsx::AnalysisCache::instance();
    auto r = c.lookup_metadata("ffffffffffffff01");
    EXPECT_FALSE(r.has_value());
}

TEST(MetadataCache, RoundTripsWidthHeightAndHeaders) {
    fitsx::CachedMetadata md;
    md.width = 6024;
    md.height = 4024;
    md.headers = {
        {"OBJECT",  "M 16 - Eagle Nebula", "target"},
        {"FILTER",  "Ha", ""},
        {"EXPTIME", "300.0", "[s] exposure"},
        {"EMPTYVAL", "", "a comment but no value"},
    };

    auto& c = fitsx::AnalysisCache::instance();
    const std::string_view key = "abcdef0123456701";
    c.store_metadata(key, md);

    auto got = c.lookup_metadata(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->width,  md.width);
    EXPECT_EQ(got->height, md.height);
    ASSERT_EQ(got->headers.size(), md.headers.size());
    for (size_t i = 0; i < md.headers.size(); ++i) {
        EXPECT_EQ(got->headers[i].key,     md.headers[i].key);
        EXPECT_EQ(got->headers[i].value,   md.headers[i].value);
        EXPECT_EQ(got->headers[i].comment, md.headers[i].comment);
    }
}

TEST(MetadataCache, RoundTripsEmptyHeaderList) {
    fitsx::CachedMetadata md;
    md.width = 100; md.height = 50;   // no headers
    auto& c = fitsx::AnalysisCache::instance();
    const std::string_view key = "abcdef0123456702";
    c.store_metadata(key, md);
    auto got = c.lookup_metadata(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->width, 100);
    EXPECT_EQ(got->height, 50);
    EXPECT_TRUE(got->headers.empty());
}

TEST(MetadataCache, StoreOverwritesPrior) {
    auto& c = fitsx::AnalysisCache::instance();
    const std::string_view key = "abcdef0123456703";
    c.store_metadata(key, {10, 20, {{"A", "1", ""}}});
    c.store_metadata(key, {30, 40, {{"B", "2", ""}, {"C", "3", ""}}});
    auto got = c.lookup_metadata(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->width, 30);
    EXPECT_EQ(got->height, 40);
    ASSERT_EQ(got->headers.size(), 2u);
    EXPECT_EQ(got->headers[0].key, "B");
    EXPECT_EQ(got->headers[1].key, "C");
}
