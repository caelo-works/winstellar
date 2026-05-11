#include "fits_core/fits_headers.h"

#include <gtest/gtest.h>

using fitsx::trim_fits_value;
using fitsx::parse_dateobs_to_filetime;

TEST(TrimFitsValue, StripsLeadingTrailingSpaces) {
    EXPECT_EQ(trim_fits_value("   42   "), "42");
}

TEST(TrimFitsValue, StripsSingleQuotes) {
    EXPECT_EQ(trim_fits_value("'M33'"), "M33");
}

TEST(TrimFitsValue, StripsQuotesAndPadding) {
    // FITS string values are space-padded inside the quotes.
    EXPECT_EQ(trim_fits_value("'M33        '"), "M33");
    EXPECT_EQ(trim_fits_value("   'M33   '   "), "M33");
}

TEST(TrimFitsValue, PassesPlainNumericThrough) {
    EXPECT_EQ(trim_fits_value("100.000"), "100.000");
}

TEST(TrimFitsValue, EmptyInput) {
    EXPECT_EQ(trim_fits_value(""),       "");
    EXPECT_EQ(trim_fits_value("    "),   "");
    EXPECT_EQ(trim_fits_value("''"),     "");
}

// --- DATE-OBS parsing ------------------------------------------------------

TEST(ParseDateObs, ValidIso8601) {
    // 2024-01-15T12:34:56 UTC — just check we get *some* FILETIME back and
    // that ordering is preserved across two known dates.
    auto a = parse_dateobs_to_filetime("2024-01-15T12:34:56");
    auto b = parse_dateobs_to_filetime("2024-01-15T12:34:57");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_LT(*a, *b);
}

TEST(ParseDateObs, WithSubsecondMillis) {
    auto a = parse_dateobs_to_filetime("2024-01-15T12:34:56");
    auto b = parse_dateobs_to_filetime("2024-01-15T12:34:56.500");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_LT(*a, *b);
}

TEST(ParseDateObs, InvalidReturnsNullopt) {
    EXPECT_FALSE(parse_dateobs_to_filetime("not a date").has_value());
    EXPECT_FALSE(parse_dateobs_to_filetime("").has_value());
    EXPECT_FALSE(parse_dateobs_to_filetime("2024-13-99T99:99:99").has_value());
}
