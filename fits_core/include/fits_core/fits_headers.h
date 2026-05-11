#pragma once

#include "fits_image.h"

#include <optional>
#include <string>

namespace fitsx {

// Trim leading/trailing whitespace and surrounding single-quotes (FITS string
// values are stored as 'value   ' in the header).
std::string trim_fits_value(std::string_view raw);

// Try to parse a FITS DATE-OBS keyword (ISO-8601-ish: YYYY-MM-DDThh:mm:ss[.sss])
// to a Windows FILETIME-compatible value (100-ns ticks since 1601-01-01 UTC).
// Returns nullopt on parse failure.
std::optional<int64_t> parse_dateobs_to_filetime(std::string_view value);

}  // namespace fitsx
