#pragma once

#include "fits_image.h"
#include "fits_loader.h"   // LoadResult

#include <cstddef>
#include <vector>

namespace fitsx {

// Camera RAW support (Nikon NEF, Canon CR2, Sony ARW, Adobe DNG, ...) backed
// by LibRaw. Dispatch is by content -- the shell extensions hand us a stream
// with no filename -- so we sniff the classic TIFF byte-order magic that every
// TIFF-derived RAW shares.

// True when the buffer begins with a little/big-endian TIFF marker
// (II*\0 / MM\0*). Cheap gate so we only spin up LibRaw on plausible RAWs;
// LibRaw itself is the final arbiter of whether the bytes actually decode.
[[nodiscard]] bool is_raw(const void* buffer, size_t size) noexcept;

// Full decode: LibRaw unpack + demosaic to a linear 16-bit RGB image with
// the camera's as-shot white balance, packed into FitsImage's three planes
// so the existing RGB render / stretch / analysis paths apply unchanged.
[[nodiscard]] LoadResult load_raw_from_memory(const void* buffer, size_t size);

// Metadata-only parse (LibRaw open_buffer, no unpack/demosaic). Used by the
// property handler so scrolling a folder of RAWs in Explorer doesn't demosaic
// every 24 Mpx frame just to fill columns. EXIF is mapped onto synthesized
// FITS keywords (EXPTIME, ISO, INSTRUME, DATE-OBS, FNUMBER, FOCALLEN) so the
// same Astro.FITS.* property plumbing surfaces it.
struct RawMetadata {
    bool success = false;
    int  width   = 0;
    int  height  = 0;
    std::vector<FitsHeader> headers;
};
[[nodiscard]] RawMetadata parse_raw_metadata(const void* buffer, size_t size);

}  // namespace fitsx
