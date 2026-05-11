#include "fits_core/fits_headers.h"
#include "fits_core/fits_image.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fitsx {

std::string trim_fits_value(std::string_view raw) {
    auto begin = raw.begin();
    auto end = raw.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    std::string s(begin, end);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        s = s.substr(1, s.size() - 2);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
    }
    return s;
}

namespace {

bool key_equal_ci(std::string_view a, const char* b) {
    const size_t bn = std::strlen(b);
    if (a.size() != bn) return false;
    for (size_t i = 0; i < bn; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::toupper(ca) != std::toupper(cb)) return false;
    }
    return true;
}

}  // namespace

const char* FitsImage::find_header(const char* key) const noexcept {
    if (!key) return nullptr;
    for (const auto& h : headers) {
        if (key_equal_ci(h.key, key)) return h.value.c_str();
    }
    return nullptr;
}

bool FitsImage::find_header(const char* key, std::string& out) const {
    const char* p = find_header(key);
    if (!p) return false;
    out = p;
    return true;
}

bool FitsImage::find_header_double(const char* key, double& out) const noexcept {
    const char* p = find_header(key);
    if (!p) return false;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    out = v;
    return true;
}

bool FitsImage::find_header_int(const char* key, long& out) const noexcept {
    const char* p = find_header(key);
    if (!p) return false;
    char* end = nullptr;
    const long v = std::strtol(p, &end, 10);
    if (end == p) return false;
    out = v;
    return true;
}

std::optional<int64_t> parse_dateobs_to_filetime(std::string_view value) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    double s = 0.0;

    char buf[64] = {0};
    const size_t n = std::min(value.size(), sizeof(buf) - 1);
    std::memcpy(buf, value.data(), n);

    int matched = std::sscanf(buf, "%4d-%2d-%2dT%2d:%2d:%lf", &Y, &M, &D, &h, &m, &s);
    if (matched < 3) {
        matched = std::sscanf(buf, "%4d-%2d-%2d", &Y, &M, &D);
        if (matched < 3) return std::nullopt;
    }
    if (Y < 1601 || M < 1 || M > 12 || D < 1 || D > 31) return std::nullopt;

    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(Y);
    st.wMonth = static_cast<WORD>(M);
    st.wDay = static_cast<WORD>(D);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(m);
    st.wSecond = static_cast<WORD>(s);
    st.wMilliseconds = static_cast<WORD>((s - static_cast<int>(s)) * 1000.0);

    FILETIME ft = {};
    if (!::SystemTimeToFileTime(&st, &ft)) return std::nullopt;
    int64_t out = (static_cast<int64_t>(ft.dwHighDateTime) << 32) |
                  static_cast<int64_t>(ft.dwLowDateTime);
    return out;
}

}  // namespace fitsx
