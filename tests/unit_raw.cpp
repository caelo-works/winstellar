#include "fits_core/raw_loader.h"

#include <gtest/gtest.h>

#include <cstdint>

// is_raw mirrors LibRaw's own identify() gate. LibRaw reads the TIFF version
// word (bytes 2..3) and discards it, so anything opening II / MM is a
// candidate whatever follows -- that is what lets Olympus, Panasonic and
// Phase One through. CR3 and RAF are not TIFF and carry their own signature.

TEST(Raw, DetectsTiffLittleEndian) {
    const uint8_t b[] = { 0x49, 0x49, 0x2A, 0x00, 0x08, 0x00 };  // "II*\0"
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, DetectsTiffBigEndian) {
    const uint8_t b[] = { 0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x08 };  // "MM\0*"
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, DetectsCanonCr3) {
    // Box size (any value) then the ISO-media brand LibRaw matches exactly,
    // trailing space included.
    const uint8_t b[] = { 0x00, 0x00, 0x00, 0x18,
                          'f', 't', 'y', 'p', 'c', 'r', 'x', ' ' };
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, RejectsOtherIsoMediaBrands) {
    // Same container shape, different brand: a QuickTime/MP4 file is not RAW.
    const uint8_t b[] = { 0x00, 0x00, 0x00, 0x18,
                          'f', 't', 'y', 'p', 'q', 't', ' ', ' ' };
    EXPECT_FALSE(fitsx::is_raw(b, sizeof b));
}

TEST(Raw, DetectsFujifilmRaf) {
    const char b[] = "FUJIFILMCCD-RAW ";
    EXPECT_TRUE(fitsx::is_raw(b, sizeof b - 1));
}

TEST(Raw, DetectsOlympusOrfVersionWords) {
    // Olympus stamps its own version word instead of 42; requiring 42 was
    // what used to reject these.
    const uint8_t iiro[] = { 'I', 'I', 'R', 'O', 0x08, 0x00 };
    const uint8_t iirs[] = { 'I', 'I', 'R', 'S', 0x08, 0x00 };
    const uint8_t mmor[] = { 'M', 'M', 'O', 'R', 0x00, 0x08 };
    EXPECT_TRUE(fitsx::is_raw(iiro, sizeof iiro));
    EXPECT_TRUE(fitsx::is_raw(iirs, sizeof iirs));
    EXPECT_TRUE(fitsx::is_raw(mmor, sizeof mmor));
}

TEST(Raw, DetectsNonStandardTiffVersionWords) {
    // Panasonic RW2 and Phase One IIQ likewise. The exact word is irrelevant:
    // LibRaw never looks at it, so neither do we.
    const uint8_t rw2[]  = { 'I', 'I', 0x55, 0x00, 0x08, 0x00 };
    const uint8_t iiq[]  = { 'I', 'I', 'I', 'I', 0x08, 0x00 };
    EXPECT_TRUE(fitsx::is_raw(rw2, sizeof rw2));
    EXPECT_TRUE(fitsx::is_raw(iiq, sizeof iiq));
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
