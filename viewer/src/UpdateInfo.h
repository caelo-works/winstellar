#pragma once

#include <string>

// Pure helpers behind the update check: version arithmetic, parsing what the
// release server tells us, and building the URLs to fetch. No Windows API and no
// network, so all of it is unit-tested -- the parts that decide whether we are
// about to run a stale or a substituted installer should not be verified by
// clicking a button.

namespace wsu {

struct Version {
    int major = 0, minor = 0, patch = 0;
    [[nodiscard]] bool valid() const noexcept { return major > 0 || minor > 0 || patch > 0; }
};

// "0.8.0", "v0.8.0", "0.8.0 (abc123, 2026-08-15)" -> {0,8,0}. Anything that does
// not start with a parseable X.Y.Z returns an invalid Version rather than a
// partially-filled one: a misparse here silently means "no update available".
[[nodiscard]] Version parse_version(const std::string& s) noexcept;

// Strictly greater, compared field by field.
[[nodiscard]] bool is_newer(const Version& candidate, const Version& current) noexcept;

// Pull the tag out of the Location header GitHub answers /releases/latest with,
// e.g. "https://github.com/caelo-works/winstellar/releases/tag/v0.8.0" -> "v0.8.0".
// Returns empty when the URL is not that shape, which is what makes a hijacked or
// unexpected redirect a no-op instead of a download.
[[nodiscard]] std::string tag_from_release_url(const std::string& location) noexcept;

// Find a file's hash in a `sha256sum`-format listing ("<64 hex>  <name>").
// Returns the lowercase hash, or empty when the file is not listed -- which must
// be treated as a failure, never as "nothing to check".
[[nodiscard]] std::string sha256_for(const std::string& sums, const std::string& filename) noexcept;

// The published asset names and URLs for a release, derived from the tag alone.
[[nodiscard]] std::string installer_name(const Version& v);
[[nodiscard]] std::string installer_url(const std::string& tag, const Version& v);
[[nodiscard]] std::string sums_url(const std::string& tag);

// True when `hex` is exactly 64 hexadecimal characters.
[[nodiscard]] bool looks_like_sha256(const std::string& hex) noexcept;

}  // namespace wsu
