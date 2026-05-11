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

// Load a FITS image from an in-memory buffer.
// CFITSIO is invoked in read-only memory mode; the caller retains ownership of
// the buffer for the duration of the call. The returned image owns its data.
[[nodiscard]] LoadResult load_from_memory(const void* buffer, size_t size);

// Convenience wrapper for the standalone viewer; takes a wide path.
[[nodiscard]] LoadResult load_from_file(const wchar_t* utf16_path);

}  // namespace fitsx
