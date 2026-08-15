#pragma once

#include "fits_image.h"

#include <functional>
#include <string>

namespace fitsx {

// FITS export.
//
// The exported FITS is meant to go back into a stacker, so the goal is fidelity,
// not convenience: where the source file is itself a usable FITS it is re-encoded
// from disk at its native BITPIX with its original CFA mosaic and its keywords
// intact. The in-memory FitsImage is only used when that is impossible, because
// by then the data has been debayered and white balanced -- the loader frees the
// CFA buffer right after demosaicing -- and is no longer the user's raw frame.

enum class FitsExportMode {
    Auto,             // re-read the source when usable, else fall back
    FromSourceOnly,   // fail rather than fall back (used after the user declined)
    FromImageOnly,    // never re-read (used after the user accepted the fallback)
};

// Why a source cannot be re-read. Surfaced so the UI can say something true.
enum class FitsSourceStatus {
    Usable,
    NoPath,            // opened from a stream, or never had a path
    Missing,           // moved, renamed or deleted since it was opened
    NotFits,           // XISF or camera RAW: there is no original FITS to copy
    Unreadable,        // present but CFITSIO will not open it
    Changed,           // dimensions no longer match what is on screen
    CompressedOrCube,  // tile-compressed or multi-frame; out of scope
    UnrotatableCfa,    // CFA pattern we cannot rotate without corrupting colour
    UnrotatableRowOrder,  // declares a row order our rotation cannot honour
};

struct FitsExportOptions {
    int          display_rotation_deg = 0;  // 0/90/180/270 clockwise, AS DISPLAYED
    std::wstring source_path;               // may be empty
    int          expect_width  = 0;         // UNROTATED image width (0 = skip check)
    int          expect_height = 0;
    std::string  source_name;               // basename, for HISTORY
    std::string  app_version;
    FitsExportMode mode = FitsExportMode::Auto;
    bool         force_memfile_sink = false;  // tests only: exercise the tier-2 sink
};

enum class FitsExportPath {
    VerbatimCopy,     // unrotated FITS source: a byte-for-byte copy
    SourceReencode,   // rotated FITS source: re-encoded from the original array
    FromImage,        // from the in-memory (debayered) image
};

struct FitsExportResult {
    bool           success   = false;
    bool           cancelled = false;
    FitsExportPath path      = FitsExportPath::FromImage;
    bool           carries_cfa = false;   // true when the original mosaic survived
    std::string    error;
};

// Can `path` be re-read as the original FITS behind the displayed image?
// Reads the header only, so it is cheap enough to call on the UI thread before
// showing a save dialog -- which is where the user should be asked, once, if
// the answer means falling back to the debayered image.
[[nodiscard]] FitsSourceStatus probe_fits_source(const std::wstring& path,
                                                 int expect_width,
                                                 int expect_height,
                                                 int display_rotation_deg = 0);

// Write `dest_path`. Never leaves a partial file: everything goes to a temp file
// in the destination directory and is renamed into place only on success.
// `is_cancelled` may be empty; when supplied it is polled between planes and
// between disk chunks.
[[nodiscard]] FitsExportResult write_fits_export(const std::wstring& dest_path,
                                                 const FitsImage& img,
                                                 const FitsExportOptions& opt,
                                                 const std::function<bool()>& is_cancelled);

}  // namespace fitsx
