#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fitsx {

// Linear-clip + Midtone-Transfer-Function parameters, in normalized [0,1]
// space relative to the data's working range. Defined here rather than in
// fits_stretch.h so FitsImage can cache its auto-stretch result without a
// circular header dependency.
struct StretchParams {
    float shadows    = 0.0f;   // black point in [0,1]
    float highlights = 1.0f;   // white point in [0,1]
    float midtone    = 0.5f;   // MTF parameter; 0.5 == identity
};

enum class PixelType {
    Unknown,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Float32,
    Float64,
};

struct FitsHeader {
    std::string key;
    std::string value;
    std::string comment;
};

// Loaded FITS image, normalized to float regardless of source bit depth.
// Only the first image HDU is loaded; multi-HDU and 3D cubes are out of scope.
struct FitsImage {
    int width = 0;
    int height = 0;
    PixelType source_type = PixelType::Unknown;

    // Pixel data, row-major, top-to-bottom (FITS-native order is bottom-to-top;
    // the loader flips to top-down to match Windows BITMAP / Direct2D convention).
    std::vector<float> data;

    // Original min/max in source units, for header display and inversion.
    double source_min = 0.0;
    double source_max = 0.0;

    // BSCALE/BZERO already applied to data. Stored for round-tripping.
    double bzero = 0.0;
    double bscale = 1.0;

    std::vector<FitsHeader> headers;

    // Cached auto-stretch params, populated by the load worker so the UI
    // thread's toolbar-Auto toggle is instant (~200 ms saved on 36 Mpx).
    // Stays empty when the loader skipped the computation.
    std::optional<StretchParams> auto_stretch;

    bool empty() const noexcept { return data.empty() || width <= 0 || height <= 0; }
    size_t pixel_count() const noexcept {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    // Returns the raw FITS keyword value (string form, trimmed).
    // Returns nullptr when not found. Pointer is owned by the FitsImage.
    const char* find_header(const char* key) const noexcept;

    bool find_header(const char* key, std::string& out) const;
    bool find_header_double(const char* key, double& out) const noexcept;
    bool find_header_int(const char* key, long& out) const noexcept;
};

}  // namespace fitsx
