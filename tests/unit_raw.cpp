#include "fits_core/raw_loader.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(Raw, DetectsTiffLittleEndian) {
    const uint8_t b[] = { 0x49, 0x49, 0x2A, 0x00, 0x08, 0x00 };  // "II*\0"
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, DetectsTiffBigEndian) {
    const uint8_t b[] = { 0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x08 };  // "MM\0*"
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, RejectsFitsMagic) {
    const char* s = "SIMPLE  =                    T";
    EXPECT_FALSE(fitsx::is_raw(s, 30));
}

TEST(Raw, RejectsXisfMagic) {
    EXPECT_FALSE(fitsx::is_raw("XISF0100", 8));
}

TEST(Raw, RejectsShortAndNull) {
    const uint8_t b[] = { 0x49, 0x49 };
    EXPECT_FALSE(fitsx::is_raw(b, sizeof b));
    EXPECT_FALSE(fitsx::is_raw(nullptr, 0));
}
