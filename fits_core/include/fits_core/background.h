#pragma once

#include "fits_image.h"

#include <vector>

namespace fitsx {

// Sky-background / illumination map -- the "fond" counterpart of the PSF plate.
// Pipeline (on linear luma):
//   per-cell 25th-percentile sky floor over a 48x72 grid
//   -> mini-ABE 2D polynomial illumination surface (object rejected upward)
//   -> radial profile (vignetting), plane fit (gradient), azimuthal anisotropy,
//      corner excess (amp glow).
// The explicit full-res source mask + dilation is dropped: the low quantile is
// already star-robust and the ABE removes the extended object.

struct BackgroundMap {
    bool success = false;
    bool skipped = false;            // image smaller than min_side
    int  rows = 0, cols = 0;         // surface grid dimensions
    std::vector<float> surface;      // rows*cols ABE illumination surface (heatmap)
    double bg_median = 0.0;

    // Radial (vignetting): centre -> corner drop and how well a revolution
    // model fits.
    double radial_drop = 0.0;        // (i_center - i_corner)/i_center
    double radial_r2 = 0.0;
    // Plane (gradient): peak-to-peak over corners / bg, and its direction.
    double plane_amplitude_rel = 0.0;
    double plane_direction_deg = 0.0;
    double plane_r2 = 0.0;
    // Azimuthal anisotropy (revolution-symmetric vignetting ~ 0; gradient high).
    double azim_aniso = 0.0;
    // Amp glow: strongest corner excess over the smooth surface (in sigma) and
    // how asymmetric the four corners are.
    double corner_excess_max = 0.0;
    double corner_asymmetry = 0.0;
};

[[nodiscard]] BackgroundMap compute_background(const FitsImage& img, int min_side = 800);

}  // namespace fitsx
