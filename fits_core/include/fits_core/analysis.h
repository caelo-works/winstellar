#pragma once

#include "fits_image.h"

#include <cstdint>

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
constexpr int kAnalysisSchemaVersion = 2;

AnalysisResult run_analysis(const FitsImage& img);

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
