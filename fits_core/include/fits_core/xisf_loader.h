#pragma once

#include "fits_image.h"
#include "fits_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fitsx {

// Subset of XISF header info we care about. Mirrors the XML <Image> element
// attributes plus the FITS keywords nested under it.
struct XisfHeaderInfo {
    int width = 0;
    int height = 0;
    int channels = 1;            // 1 (Gray) or 3 (RGB)
    PixelType sample_format = PixelType::Unknown;
    bool color_rgb = false;

    // Where the binary pixel block lives in the file:
    uint64_t pixel_offset = 0;
    uint64_t pixel_size = 0;
    bool compressed = false;     // true = compression codec (we don't yet support these)
    std::string compression;     // e.g. "zlib", "lz4", "zstd" if any

    // Channels are stored planar in XISF: R block, then G block, then B block.
    // For Float32 mono: pixel_offset .. pixel_offset+W*H*4.

    std::vector<FitsHeader> fits_keywords;

    bool empty() const noexcept { return width <= 0 || height <= 0; }
};

// Detect XISF magic 'XISF0100' in the first 8 bytes.
bool is_xisf(const void* buffer, size_t size) noexcept;

// Parse the XML header of an in-memory XISF file. Buffer must contain at
// least the magic + length prefix + full XML header (16 + header_length
// bytes). Returns success=false if the buffer is too small or malformed; the
// caller can then read more bytes and retry.
struct XisfParseResult {
    bool success = false;
    std::string error;
    XisfHeaderInfo header;
    uint32_t xml_length = 0;     // bytes 8..12 of the file
};
XisfParseResult parse_xisf_header(const void* buffer, size_t size);

// Load a full XISF image from an in-memory buffer (header + pixel data both
// present). Currently supports uncompressed Float32 mono and RGB; for RGB
// only channel 0 (R) is exposed in FitsImage.data — RGB compositing is left
// to a later iteration. Returns LoadResult mirroring fits_loader.h.
LoadResult load_xisf_from_memory(const void* buffer, size_t size);

}  // namespace fitsx
