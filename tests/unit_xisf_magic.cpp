#include "fits_core/xisf_loader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using fitsx::is_xisf;

TEST(IsXisf, DetectsMagicAtStart) {
    const char buf[] = "XISF0100extra data here";
    EXPECT_TRUE(is_xisf(buf, sizeof(buf) - 1));
}

TEST(IsXisf, RejectsWrongMagic) {
    const char buf[] = "XISF0200extra";   // wrong version digit (we accept 0100 only)
    EXPECT_FALSE(is_xisf(buf, sizeof(buf) - 1));
}

TEST(IsXisf, RejectsRandomBytes) {
    const char buf[] = "SIMPLE  =                    T";   // FITS header start
    EXPECT_FALSE(is_xisf(buf, sizeof(buf) - 1));
}

TEST(IsXisf, RejectsBufferTooShort) {
    const char buf[] = "XISF";
    EXPECT_FALSE(is_xisf(buf, sizeof(buf) - 1));
}

TEST(IsXisf, RejectsNullBuffer) {
    EXPECT_FALSE(is_xisf(nullptr, 64));
}

TEST(IsXisf, BoundaryExactMagic) {
    const char buf[8] = { 'X','I','S','F','0','1','0','0' };
    EXPECT_TRUE(is_xisf(buf, sizeof(buf)));
}
