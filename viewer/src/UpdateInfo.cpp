#include "UpdateInfo.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace wsu {

namespace {

// The repository the updater trusts. Hard-coded on purpose: the installer URL is
// never taken from a server response, only built from a tag we validated, so a
// tampered redirect cannot point the download somewhere else.
constexpr const char* kOwnerRepo = "caelo-works/winstellar";

bool is_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

Version parse_version(const std::string& s) noexcept {
    Version v;
    size_t i = 0;
    while (i < s.size() && (s[i] == 'v' || s[i] == 'V' || std::isspace(static_cast<unsigned char>(s[i]))))
        ++i;

    auto number = [&](int& out) -> bool {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        long n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            n = n * 10 + (s[i] - '0');
            if (n > 100000) return false;      // not a version, something else
            ++i;
        }
        out = static_cast<int>(n);
        return true;
    };

    if (!number(v.major)) return {};
    if (i >= s.size() || s[i] != '.') return {};
    ++i;
    if (!number(v.minor)) return {};
    if (i >= s.size() || s[i] != '.') return {};
    ++i;
    if (!number(v.patch)) return {};
    // Anything after the patch number (a space, a build suffix) is ignored, but a
    // fourth dotted component means this is not the version string we think.
    if (i < s.size() && s[i] == '.') return {};
    return v;
}

bool is_newer(const Version& c, const Version& cur) noexcept {
    if (!c.valid()) return false;
    if (c.major != cur.major) return c.major > cur.major;
    if (c.minor != cur.minor) return c.minor > cur.minor;
    return c.patch > cur.patch;
}

std::string tag_from_release_url(const std::string& location) noexcept {
    // Only accept the exact shape we expect from our own repository. Anything
    // else -- a different host, a different repo, a path we do not recognise --
    // yields no tag, so nothing is downloaded.
    const std::string prefix =
        std::string("https://github.com/") + kOwnerRepo + "/releases/tag/";
    if (location.compare(0, prefix.size(), prefix) != 0) return {};
    std::string tag = location.substr(prefix.size());
    // Strip a query or fragment, then reject anything that is not a bare tag.
    const size_t cut = tag.find_first_of("?#/");
    if (cut != std::string::npos) tag = tag.substr(0, cut);
    if (tag.empty() || !parse_version(tag).valid()) return {};
    for (char c : tag)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return {};
    return tag;
}

std::string sha256_for(const std::string& sums, const std::string& filename) noexcept {
    std::istringstream in(sums);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string hash = line.substr(0, sp);
        std::string name = line.substr(sp);
        // sha256sum writes two spaces (or " *" in binary mode) before the name.
        size_t j = 0;
        while (j < name.size() && (name[j] == ' ' || name[j] == '*')) ++j;
        name = name.substr(j);
        if (name == filename) {
            for (char& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return looks_like_sha256(hash) ? hash : std::string{};
        }
    }
    return {};
}

std::string installer_name(const Version& v) {
    return "WinStellarSetup-" + std::to_string(v.major) + "." +
           std::to_string(v.minor) + "." + std::to_string(v.patch) + ".exe";
}

std::string installer_url(const std::string& tag, const Version& v) {
    return std::string("https://github.com/") + kOwnerRepo + "/releases/download/" +
           tag + "/" + installer_name(v);
}

std::string sums_url(const std::string& tag) {
    return std::string("https://github.com/") + kOwnerRepo + "/releases/download/" +
           tag + "/SHA256SUMS.txt";
}

bool looks_like_sha256(const std::string& hex) noexcept {
    if (hex.size() != 64) return false;
    for (char c : hex) if (!is_hex(c)) return false;
    return true;
}

}  // namespace wsu
