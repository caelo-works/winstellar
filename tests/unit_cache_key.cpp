#include "fits_core/analysis.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using fitsx::compute_cache_key;

namespace {

// compute_cache_key reads up to min(size, kCacheKeySampleBytes = 64 KB) bytes
// from the buffer. Tests must pass a buffer at least that large; otherwise we
// read past the end and the result depends on unrelated stack/heap contents.
constexpr size_t kHashedBytes = 64 * 1024;

}  // namespace

TEST(CacheKey, IsThirtyTwoLowercaseHex) {
    std::vector<uint8_t> buf(kHashedBytes, 0xAB);
    const std::string k = compute_cache_key(buf.data(), buf.size());
    EXPECT_EQ(k.size(), 32u);   // 128-bit key (two FNV-1a lanes)
    for (char c : k) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-hex char " << c << " in key " << k;
    }
}

TEST(CacheKey, DeterministicForSameInput) {
    std::vector<uint8_t> buf(kHashedBytes, 0x00);
    buf[0] = 0x01; buf[1] = 0x02; buf[2] = 0x03; buf[3] = 0x04;
    const std::string a = compute_cache_key(buf.data(), buf.size());
    const std::string b = compute_cache_key(buf.data(), buf.size());
    EXPECT_EQ(a, b);
}

TEST(CacheKey, ChangesWithContent) {
    std::vector<uint8_t> a(kHashedBytes, 0xAA);
    std::vector<uint8_t> b = a;
    b[5] = 0xFF;
    EXPECT_NE(compute_cache_key(a.data(), a.size()),
              compute_cache_key(b.data(), b.size()));
}

TEST(CacheKey, ChangesWithSize) {
    // Same hashed bytes, different declared file size → different key.
    // The size is mixed in after the bytes so a 32 MB partial read of a
    // 100 MB file doesn't collide with one taken from a 200 MB file.
    std::vector<uint8_t> buf(kHashedBytes, 0x55);
    EXPECT_NE(compute_cache_key(buf.data(), 100ull * 1024 * 1024),
              compute_cache_key(buf.data(), 200ull * 1024 * 1024));
}
