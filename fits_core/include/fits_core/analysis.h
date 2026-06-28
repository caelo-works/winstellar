#pragma once

#include "fits_image.h"

#include <cstdint>
#include <vector>

namespace fitsx {

// Per-frame quality + statistics. All values in source units (not normalized
// to [0,1]) so they read like NINA / ASTAP.
struct AnalysisResult {
    bool success = false;

    // Stellar analysis (medians across detected stars).
    int star_count = 0;
    double hfr_median = 0.0;
    double hfr_stddev = 0.0;
    double fwhm_median = 0.0;
    double eccentricity_median = 0.0;

    // Whole-image pixel statistics.
    double mean = 0.0;
    double stddev = 0.0;
    double median = 0.0;
    double mad = 0.0;
    double min_value = 0.0;
    uint64_t min_count = 0;     // pixels exactly equal to min_value
    double max_value = 0.0;
    uint64_t max_count = 0;     // pixels at max_value -> saturation indicator
};

// Bumped whenever any metric definition changes. Old cache rows below this
// version are ignored and recomputed.
// v2: a regression in PropertyHandler caused 0-star analyses to be cached
//     for FITS after the XISF refactor (run_analysis was being called on a
//     moved-from image). Bump to invalidate those bogus entries.
// v3: robust star detector (local-background mesh + roundness/sharpness/SNR
//     cuts, saturation-aware) replaced the global median+5sigma + 4..200px
//     filter. Star counts / HFR / eccentricity all shift, so old rows must go.
constexpr int kAnalysisSchemaVersion = 3;

[[nodiscard]] AnalysisResult run_analysis(const FitsImage& img);

// ---------------------------------------------------------------------------
//  Detailed (per-star) analysis -- the foundation for the inspection tools
//  (star overlay, tilt map, aberration vectors). run_analysis keeps returning
//  cheap medians for the cache / Explorer columns; this returns the full star
//  list and is computed on demand by the viewer, never cached.
// ---------------------------------------------------------------------------

// One detected star, in display-image pixel coordinates (top-left origin, the
// same space the renderer draws the bitmap in -- so overlays map 1:1).
struct DetectedStar {
    float x = 0.0f;       // flux-weighted centroid X
    float y = 0.0f;       // flux-weighted centroid Y
    float flux = 0.0f;    // background-subtracted integrated flux
    float hfr = 0.0f;     // half-flux radius (px)
    float fwhm = 0.0f;    // px, from second moments
    float ecc = 0.0f;     // eccentricity 0 (round) .. ->1 (elongated)
    float theta = 0.0f;   // major-axis orientation, radians; atan2 convention
                          // in image space (y grows downward)
};

struct DetailedAnalysis {
    bool success = false;
    int  width = 0;
    int  height = 0;
    double background = 0.0;   // median (detection floor)
    double threshold = 0.0;    // median + 5*sigma_mad
    std::vector<DetectedStar> stars;
};

[[nodiscard]] DetailedAnalysis run_detailed_analysis(const FitsImage& img);

// ---------------------------------------------------------------------------
//  Tilt map -- bins the detected stars into a grid x grid mesh and reports
//  median HFR per cell. Sensor / focuser tilt shows as a sharp corner opposite
//  a soft one. Pure function of a DetailedAnalysis so it is unit-testable.
// ---------------------------------------------------------------------------

struct TiltCell {
    int    count = 0;          // stars binned into this cell
    double hfr_median = 0.0;   // 0 when count == 0
    double ecc_mean = 0.0;
};

struct TiltResult {
    bool success = false;
    int  grid = 0;                  // grid x grid cells, row-major in `cells`
    std::vector<TiltCell> cells;
    double hfr_min = 0.0;           // across populated cells
    double hfr_max = 0.0;
    double tilt_pct = 0.0;          // (max-min)/min * 100 over the 4 corner cells
    double curvature_pct = 0.0;     // (mean corners - center)/center * 100; 0 if
                                    // the centre cell is empty. >0 => corners
                                    // softer than centre (field curvature).
    int    best_corner = -1;        // 0=TL 1=TR 2=BL 3=BR (lowest corner HFR)
    int    worst_corner = -1;       // highest corner HFR
};

[[nodiscard]] TiltResult compute_tilt(const DetailedAnalysis& a, int grid = 3);

// Stable 64-bit FNV-1a hash of (first 8 KB of buffer || size_be). Returned as
// 16 lowercase hex characters. Used as the analysis-cache primary key — a
// FITS file's first 8 KB always covers the full primary header, which is
// unique per capture, so collisions are astronomically unlikely.
//
// NOTE: pass the FILE'S TOTAL SIZE as the second argument, not the buffer's
// length. The function only reads up to 8 KB from `buffer`. Using total file
// size keeps the key stable across partial reads (PropertyHandler reads only
// 32 MB, the viewer reads the whole file - both must produce the same key).
std::string compute_cache_key(const void* buffer, size_t size) noexcept;

// Convenience wrapper for the standalone viewer: opens the file, hashes its
// first 8 KB, returns the stable cache key. Empty string on any I/O error.
std::string compute_cache_key_from_file(const wchar_t* utf16_path);

}  // namespace fitsx
