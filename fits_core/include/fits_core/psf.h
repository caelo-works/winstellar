#pragma once

#include "fits_image.h"

#include <vector>

namespace fitsx {

// PSF "plate" measurement.
// Per zone, detect the best stars (local maxima
// gated by SNR / concentration / hot-pixel rejection / FWHM range), then measure
// elongation with ADAPTIVE GAUSSIAN-WEIGHTED second moments (iterative weights
// crush the noisy wings that bias raw moments). All measurement is done on the
// LINEAR image (luma), never on a stretched render.

constexpr int kPsfStamp = 29;   // CUT: per-star stamp side (px), odd

// One measured star.
struct PsfStar {
    int   cx = 0, cy = 0;       // integer local-max position, full-image px
    int   ox = 0, oy = 0;       // stamp top-left in full-image px (kPsfStamp box)
    float x0 = 0, y0 = 0;       // adaptive centroid within the stamp (px)
    float fwhm = 0;
    float ecc = 0;              // 0 round .. ->1 elongated
    float elong = 0;            // a/b axis ratio (>=1)
    float pa = 0;               // major-axis position angle, radians (image y-down)
    float siga = 0, sigb = 0;   // sigma major / minor (px)
    float snr = 0;              // peak / local noise
};

enum class PsfAxis { None, Ref, Radial, Tangential, Oblique };

// One zone (grid cell): up to nstar measured stars + aggregates.
struct PsfZone {
    int gx = 0, gy = 0;
    std::vector<PsfStar> stars;
    float elong_median = 0;
    float ecc_median = 0;
    float pa_median = 0;        // circular median of the star PAs
    PsfAxis axis = PsfAxis::None;
};

struct PsfPlate {
    bool success = false;
    int  grid = 0;
    int  nstar = 0;
    int  width = 0, height = 0;
    std::vector<PsfZone> zones;   // grid*grid, row-major
};

// grid x grid zones, up to nstar stars each. Cheap (works on small per-zone
// regions); safe to call on the UI thread when the inspector opens.
[[nodiscard]] PsfPlate compute_psf_plate(const FitsImage& img,
                                         int grid = 3, int nstar = 4);

}  // namespace fitsx
