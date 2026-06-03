#pragma once

#include "fits_image.h"

#include <cstddef>
#include <string>

namespace fitsx {

struct LoadResult {
    bool success = false;
    std::string error;
    FitsImage image;
};

// Container format of an in-memory image buffer, decided by magic bytes.
// Single source of truth for format priority so every dispatch site (the
// loader below and the shell property handler) agrees on detection + ordering.
enum class ImageFormat { Fits, Xisf, Raw };
[[nodiscard]] ImageFormat detect_format(const void* buffer, size_t size) noexcept;

// Load a FITS image from an in-memory buffer.
// CFITSIO is invoked in read-only memory mode; the caller retains ownership of
// the buffer for the duration of the call. The returned image owns its data.
[[nodiscard]] LoadResult load_from_memory(const void* buffer, size_t size);

// Convenience wrapper for the standalone viewer; takes a wide path.
[[nodiscard]] LoadResult load_from_file(const wchar_t* utf16_path);

}  // namespace fitsx
