#pragma once

#include "fits_image.h"

#include <cstddef>
#include <string>
#include <vector>

namespace fitsx {

struct LoadResult {
    bool success = false;
    std::string error;
    FitsImage image;
};

// Header-only FITS parse: dimensions + keywords, WITHOUT reading pixel data.
// Cheap and bounded (reads only the header blocks), so it is safe to run in the
// in-process Explorer property handler on the 32 MB-capped buffer even for
// large frames whose pixels extend past the cap.
struct FitsMetadata {
    bool success = false;
    int  width  = 0;
    int  height = 0;
    std::vector<FitsHeader> headers;
    std::string error;
};
[[nodiscard]] FitsMetadata parse_fits_metadata(const void* buffer, size_t size);

// Container format of an in-memory image buffer, decided by magic bytes.
// Single source of truth for format priority so every dispatch site (the
// loader below and the shell property handler) agrees on detection + ordering.
enum class ImageFormat { Fits, Xisf, Raw };
[[nodiscard]] ImageFormat detect_format(const void* buffer, size_t size) noexcept;

// Load a FITS image from an in-memory buffer.
// CFITSIO is invoked in read-only memory mode; the caller retains ownership of
// the buffer for the duration of the call. The returned image owns its data.
//
// prefer_fast=true asks for a speed/size-optimized decode suitable for a
// thumbnail (the result is downsampled anyway). Currently honored by the RAW
// path (LibRaw half_size, ~4x faster); FITS/XISF decode full-resolution for now.
[[nodiscard]] LoadResult load_from_memory(const void* buffer, size_t size,
                                          bool prefer_fast = false);

// Convenience wrapper for the standalone viewer; takes a wide path.
[[nodiscard]] LoadResult load_from_file(const wchar_t* utf16_path);

}  // namespace fitsx
