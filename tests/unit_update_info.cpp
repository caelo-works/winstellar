// The decisions the updater makes before it downloads and runs an installer.
//
// Every one of these is a security boundary: a version comparison that says "no
// update" leaves users on a vulnerable parser, and a redirect or checksum parsed
// too loosely is how an updater is turned into a delivery channel. None of it is
// left to a click-through.

#include "UpdateInfo.h"

#include <gtest/gtest.h>

#include <string>

using wsu::Version;
using wsu::parse_version;
using wsu::is_newer;
using wsu::tag_from_release_url;
using wsu::sha256_for;

namespace {
constexpr const char* kSums =
    "ee4316d44b93cd9bcdeb8fee20cf60a11487f379def843e501eba1a9ddf057cd  WinStellarSetup-0.8.0.exe\n";
}

// ------------------------------------------------------------- versions ----

TEST(UpdateInfo, ParsesTheVersionsWeActuallyShip) {
    EXPECT_EQ(parse_version("0.8.0").minor, 8);
    EXPECT_EQ(parse_version("v0.8.0").minor, 8);
    EXPECT_EQ(parse_version("0.6.10").patch, 10);
    // FITS_VERSION_FULL_STR shape, as embedded in the binary.
    const Version v = parse_version("0.8.0 (a7f996c3, 2026-08-15)");
    EXPECT_EQ(v.major, 0);
    EXPECT_EQ(v.minor, 8);
    EXPECT_EQ(v.patch, 0);
}

TEST(UpdateInfo, RejectsAnythingThatIsNotAVersion) {
    // A partial parse would be worse than none: it would silently compare wrong.
    for (const char* s : {"", "v", "0", "0.8", "x.y.z", "0.8.0.1", "latest",
                          "../../evil", "999999.0.0"})
        EXPECT_FALSE(parse_version(s).valid()) << s;
}

TEST(UpdateInfo, ComparesFieldByFieldNotLexically) {
    // The case a string comparison gets wrong, and the reason 0.6.10 exists.
    EXPECT_TRUE(is_newer(parse_version("0.6.10"), parse_version("0.6.9")));
    EXPECT_FALSE(is_newer(parse_version("0.6.9"), parse_version("0.6.10")));
    EXPECT_TRUE(is_newer(parse_version("0.8.0"), parse_version("0.7.9")));
    EXPECT_TRUE(is_newer(parse_version("1.0.0"), parse_version("0.99.99")));
}

TEST(UpdateInfo, SameVersionIsNotAnUpdate) {
    EXPECT_FALSE(is_newer(parse_version("0.8.0"), parse_version("0.8.0")));
}

TEST(UpdateInfo, ADowngradeIsNeverOffered) {
    // A rolled-back or hijacked "latest" must not push users backwards onto a
    // version whose vulnerabilities are already public.
    EXPECT_FALSE(is_newer(parse_version("0.7.0"), parse_version("0.8.0")));
    EXPECT_FALSE(is_newer(parse_version("0.0.1"), parse_version("0.8.0")));
    EXPECT_FALSE(is_newer(Version{}, parse_version("0.8.0")));
}

// ------------------------------------------------------------- redirect ----

TEST(UpdateInfo, AcceptsOnlyOurOwnReleaseUrl) {
    EXPECT_EQ(tag_from_release_url(
        "https://github.com/caelo-works/winstellar/releases/tag/v0.8.0"), "v0.8.0");
}

TEST(UpdateInfo, RejectsARedirectAnywhereElse) {
    // The redirect is attacker-influenceable in a way the rest is not, so it is
    // the one input that gets an allow-list rather than a sanity check.
    for (const char* url : {
        "https://github.com/someone-else/winstellar/releases/tag/v9.9.9",
        "https://github.com/caelo-works/other/releases/tag/v9.9.9",
        "https://evil.example/caelo-works/winstellar/releases/tag/v9.9.9",
        "http://github.com/caelo-works/winstellar/releases/tag/v9.9.9",   // not TLS
        "https://github.com/caelo-works/winstellar/releases/tag/",
        "https://github.com/caelo-works/winstellar/releases/tag/../../evil",
        "https://github.com/caelo-works/winstellar/releases/latest",
        "",
    })
        EXPECT_TRUE(tag_from_release_url(url).empty()) << url;
}

TEST(UpdateInfo, StripsQueryAndFragmentFromTheTag) {
    EXPECT_EQ(tag_from_release_url(
        "https://github.com/caelo-works/winstellar/releases/tag/v0.8.0?utm=x"), "v0.8.0");
    EXPECT_EQ(tag_from_release_url(
        "https://github.com/caelo-works/winstellar/releases/tag/v0.8.0#notes"), "v0.8.0");
}

// ------------------------------------------------------------ checksums ----

TEST(UpdateInfo, FindsTheHashForTheRequestedFile) {
    EXPECT_EQ(sha256_for(kSums, "WinStellarSetup-0.8.0.exe"),
              "ee4316d44b93cd9bcdeb8fee20cf60a11487f379def843e501eba1a9ddf057cd");
}

TEST(UpdateInfo, MissingOrMalformedHashesYieldNothing) {
    // Empty must mean "refuse", never "skip the check".
    EXPECT_TRUE(sha256_for(kSums, "WinStellarSetup-0.9.0.exe").empty());
    EXPECT_TRUE(sha256_for("", "WinStellarSetup-0.8.0.exe").empty());
    EXPECT_TRUE(sha256_for("notahash  WinStellarSetup-0.8.0.exe", "WinStellarSetup-0.8.0.exe").empty());
    EXPECT_TRUE(sha256_for("ee4316  WinStellarSetup-0.8.0.exe", "WinStellarSetup-0.8.0.exe").empty());
}

TEST(UpdateInfo, HandlesMultiLineAndBinaryMarkedListings) {
    const std::string sums =
        "1111111111111111111111111111111111111111111111111111111111111111  other.exe\r\n"
        "ee4316d44b93cd9bcdeb8fee20cf60a11487f379def843e501eba1a9ddf057cd *WinStellarSetup-0.8.0.exe\r\n";
    EXPECT_EQ(sha256_for(sums, "WinStellarSetup-0.8.0.exe"),
              "ee4316d44b93cd9bcdeb8fee20cf60a11487f379def843e501eba1a9ddf057cd");
    EXPECT_EQ(sha256_for(sums, "other.exe").substr(0, 4), "1111");
}

TEST(UpdateInfo, HashComparisonIsCaseInsensitiveOnTheFileSide) {
    const std::string upper =
        "EE4316D44B93CD9BCDEB8FEE20CF60A11487F379DEF843E501EBA1A9DDF057CD  WinStellarSetup-0.8.0.exe";
    EXPECT_EQ(sha256_for(upper, "WinStellarSetup-0.8.0.exe"),
              "ee4316d44b93cd9bcdeb8fee20cf60a11487f379def843e501eba1a9ddf057cd");
}

// ----------------------------------------------------------------- URLs ----

TEST(UpdateInfo, UrlsAreBuiltFromTheTagNotTakenFromTheServer) {
    const Version v = parse_version("0.8.0");
    EXPECT_EQ(wsu::installer_name(v), "WinStellarSetup-0.8.0.exe");
    EXPECT_EQ(wsu::installer_url("v0.8.0", v),
              "https://github.com/caelo-works/winstellar/releases/download/v0.8.0/WinStellarSetup-0.8.0.exe");
    EXPECT_EQ(wsu::sums_url("v0.8.0"),
              "https://github.com/caelo-works/winstellar/releases/download/v0.8.0/SHA256SUMS.txt");
}

TEST(UpdateInfo, Sha256ShapeIsEnforced) {
    EXPECT_TRUE(wsu::looks_like_sha256(std::string(64, 'a')));
    EXPECT_FALSE(wsu::looks_like_sha256(std::string(63, 'a')));
    EXPECT_FALSE(wsu::looks_like_sha256(std::string(65, 'a')));
    EXPECT_FALSE(wsu::looks_like_sha256(std::string(64, 'z')));
    EXPECT_FALSE(wsu::looks_like_sha256(""));
}
