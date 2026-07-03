#include "fits_core/raw_loader.h"
#include "fits_core/fits_loader.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// The decode tests need a real camera RAW (NEF/CR2/...): a valid RAW can't be
// synthesized cheaply. Point WINSTELLAR_NEF at one to run them; CI (which has
// no RAW in-tree) skips. Locally this is set to the testset's DSC_0026.NEF.
std::string raw_test_path() {
    size_t len = 0;
    char buf[1024] = {0};
    if (getenv_s(&len, buf, sizeof buf, "WINSTELLAR_NEF") == 0 && len > 1)
        return std::string(buf);
    return {};
}

std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

}  // namespace

TEST(RawIntegration, DecodesRawToLinearColor) {
    const auto p = raw_test_path();
    if (p.empty() || !std::filesystem::exists(p))
        GTEST_SKIP() << "Set WINSTELLAR_NEF to a camera RAW file to run this test";

    auto bytes = read_file(p);
    ASSERT_FALSE(bytes.empty());
    ASSERT_TRUE(fitsx::is_raw(bytes.data(), bytes.size()));

    auto r = fitsx::load_from_memory(bytes.data(), bytes.size());
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_GT(r.image.width, 1000);
    EXPECT_GT(r.image.height, 1000);
    EXPECT_TRUE(r.image.is_rgb());                 // demosaiced to RGB
    EXPECT_GT(r.image.source_max, r.image.source_min);

    // EXIF was mapped onto synthesized FITS keywords.
    std::string instr;
    EXPECT_TRUE(r.image.find_header("INSTRUME", instr));
    EXPECT_FALSE(instr.empty());
}

TEST(RawIntegration, HalfResDecodeIsAboutHalfSize) {
    const auto p = raw_test_path();
    if (p.empty() || !std::filesystem::exists(p))
        GTEST_SKIP() << "Set WINSTELLAR_NEF to a camera RAW file to run this test";

    auto bytes = read_file(p);
    ASSERT_FALSE(bytes.empty());

    auto full = fitsx::load_raw_from_memory(bytes.data(), bytes.size(), /*half_res=*/false);
    auto half = fitsx::load_raw_from_memory(bytes.data(), bytes.size(), /*half_res=*/true);
    ASSERT_TRUE(full.success) << full.error;
    ASSERT_TRUE(half.success) << half.error;
    EXPECT_TRUE(half.image.is_rgb());

    // Half-size decode is ~half the linear resolution (a quarter of the pixels).
    // LibRaw rounds, so allow a couple of pixels of slack.
    EXPECT_NEAR(half.image.width,  full.image.width  / 2, 2);
    EXPECT_NEAR(half.image.height, full.image.height / 2, 2);
    EXPECT_LT(static_cast<size_t>(half.image.width) * half.image.height,
              static_cast<size_t>(full.image.width) * full.image.height / 3);  // ~1/4
}

TEST(RawIntegration, MetadataOnlyParsesWithoutDemosaic) {
    const auto p = raw_test_path();
    if (p.empty() || !std::filesystem::exists(p))
        GTEST_SKIP() << "Set WINSTELLAR_NEF to a camera RAW file to run this test";

    auto bytes = read_file(p);
    auto m = fitsx::parse_raw_metadata(bytes.data(), bytes.size());
    ASSERT_TRUE(m.success);
    EXPECT_GT(m.width, 0);
    EXPECT_GT(m.height, 0);
    // At least the camera model should come through.
    bool has_instr = false;
    for (const auto& h : m.headers) if (h.key == "INSTRUME") has_instr = true;
    EXPECT_TRUE(has_instr);
}
