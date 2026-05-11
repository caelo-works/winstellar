#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wst {

// Build a minimal valid XISF byte buffer (signature + header length + XML +
// pixel block) for a mono Float32 image of the given dimensions, with an
// optional FITS keyword embedded.
//
// Layout produced:
//   bytes 0..7   : "XISF0100" magic
//   bytes 8..11  : header_length (uint32 LE) = XML text byte count
//   bytes 12..15 : reserved = 0
//   bytes 16..16+header_length : XML header
//   following   : raw Float32 pixel data, w*h floats little-endian
//
// The buffer is returned ready to be passed to fitsx::load_xisf_from_memory
// (no compression, single channel "Gray", uncompressed).
std::vector<uint8_t> make_synth_xisf(int width, int height,
                                     const std::vector<float>& pixels,
                                     const std::string& fits_keyword_name  = {},
                                     const std::string& fits_keyword_value = {});

}  // namespace wst
