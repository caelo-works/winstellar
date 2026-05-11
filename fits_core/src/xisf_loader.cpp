#include "fits_core/xisf_loader.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace fitsx {

namespace {

constexpr size_t kMagicSize = 8;
constexpr size_t kHeaderPrefixSize = 16;   // magic(8) + xml_len(4) + reserved(4)

PixelType parse_sample_format(const std::string& s) {
    if (s == "UInt8")    return PixelType::UInt8;
    if (s == "UInt16")   return PixelType::UInt16;
    if (s == "UInt32")   return PixelType::UInt32;
    if (s == "Int16")    return PixelType::Int16;
    if (s == "Int32")    return PixelType::Int32;
    if (s == "Float32")  return PixelType::Float32;
    if (s == "Float64")  return PixelType::Float64;
    return PixelType::Unknown;
}

size_t bytes_per_sample(PixelType t) {
    switch (t) {
        case PixelType::UInt8:   return 1;
        case PixelType::Int16:
        case PixelType::UInt16:  return 2;
        case PixelType::Int32:
        case PixelType::UInt32:
        case PixelType::Float32: return 4;
        case PixelType::Float64: return 8;
        default:                 return 0;
    }
}

// Parse "attachment:OFFSET:SIZE[:COMPRESSION:UNCOMP_SIZE]"
bool parse_location(const std::string& loc,
                    uint64_t& offset, uint64_t& size,
                    std::string& compression) {
    if (loc.rfind("attachment:", 0) != 0) return false;
    std::string s = loc.substr(11);
    std::vector<std::string> parts;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(':', p);
        if (q == std::string::npos) { parts.push_back(s.substr(p)); break; }
        parts.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    if (parts.size() < 2) return false;
    try {
        offset = std::stoull(parts[0]);
        size = std::stoull(parts[1]);
    } catch (...) { return false; }
    if (parts.size() >= 3) compression = parts[2];
    return true;
}

// Parse "W:H:C" geometry attribute.
bool parse_geometry(const std::string& g, int& w, int& h, int& c) {
    std::stringstream ss(g);
    char colon1 = 0, colon2 = 0;
    return static_cast<bool>(ss >> w >> colon1 >> h >> colon2 >> c) &&
           colon1 == ':' && colon2 == ':';
}

// FITS keyword values in XISF XML are stored as the FITS-style raw value:
//   string keywords come quoted: <FITSKeyword name="OBJECT" value="'M33'"/>
//   numeric: <FITSKeyword name="EXPTIME" value="100.000"/>
// We trim the surrounding quotes / whitespace so downstream lookups (which
// reuse the FITS path) see a clean value.
std::string trim_xisf_value(const std::string& raw) {
    size_t a = 0, b = raw.size();
    while (a < b && (raw[a] == ' ' || raw[a] == '\t')) ++a;
    while (b > a && (raw[b - 1] == ' ' || raw[b - 1] == '\t')) --b;
    if (b - a >= 2 && raw[a] == '\'' && raw[b - 1] == '\'') {
        ++a; --b;
        while (b > a && raw[b - 1] == ' ') --b;   // FITS strings are space-padded
    }
    return raw.substr(a, b - a);
}

}  // namespace

bool is_xisf(const void* buffer, size_t size) noexcept {
    if (!buffer || size < kMagicSize) return false;
    return std::memcmp(buffer, "XISF0100", kMagicSize) == 0;
}

XisfParseResult parse_xisf_header(const void* buffer, size_t size) {
    XisfParseResult res;
    if (!is_xisf(buffer, size)) {
        res.error = "Not an XISF file (magic mismatch)";
        return res;
    }
    if (size < kHeaderPrefixSize) {
        res.error = "Buffer too small for XISF header prefix";
        return res;
    }
    const auto* p = static_cast<const uint8_t*>(buffer);
    uint32_t xml_len = 0;
    std::memcpy(&xml_len, p + kMagicSize, 4);
    res.xml_length = xml_len;

    if (size < kHeaderPrefixSize + xml_len) {
        res.error = "Buffer truncated before XML header end";
        return res;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result pr = doc.load_buffer(
        p + kHeaderPrefixSize, xml_len, pugi::parse_default, pugi::encoding_utf8);
    if (!pr) {
        res.error = std::string("XML parse error: ") + pr.description();
        return res;
    }
    pugi::xml_node root = doc.child("xisf");
    if (!root) {
        res.error = "Root <xisf> element missing";
        return res;
    }
    pugi::xml_node img = root.child("Image");
    if (!img) {
        res.error = "<Image> element missing";
        return res;
    }

    auto& h = res.header;
    const std::string geometry = img.attribute("geometry").as_string();
    if (!parse_geometry(geometry, h.width, h.height, h.channels)) {
        res.error = "Bad geometry attribute: " + geometry;
        return res;
    }

    h.sample_format = parse_sample_format(img.attribute("sampleFormat").as_string());
    if (h.sample_format == PixelType::Unknown) {
        res.error = "Unsupported sampleFormat";
        return res;
    }
    const std::string color = img.attribute("colorSpace").as_string();
    h.color_rgb = (color == "RGB");

    const std::string loc = img.attribute("location").as_string();
    if (!parse_location(loc, h.pixel_offset, h.pixel_size, h.compression)) {
        res.error = "Bad location attribute: " + loc;
        return res;
    }
    h.compressed = !h.compression.empty();

    // Collect FITSKeyword children. XISF keywords mirror FITS exactly so we
    // can reuse the FitsImage::find_header lookups downstream unchanged.
    for (pugi::xml_node kw = img.child("FITSKeyword"); kw;
         kw = kw.next_sibling("FITSKeyword")) {
        FitsHeader fh;
        fh.key     = kw.attribute("name").as_string();
        fh.value   = trim_xisf_value(kw.attribute("value").as_string());
        fh.comment = kw.attribute("comment").as_string();
        if (!fh.key.empty()) h.fits_keywords.push_back(std::move(fh));
    }

    res.success = true;
    return res;
}

LoadResult load_xisf_from_memory(const void* buffer, size_t size) {
    LoadResult out;
    auto parsed = parse_xisf_header(buffer, size);
    if (!parsed.success) { out.error = std::move(parsed.error); return out; }
    const XisfHeaderInfo& h = parsed.header;

    if (h.compressed) {
        out.error = "Compressed XISF (" + h.compression + ") not yet supported";
        return out;
    }
    const size_t bps = bytes_per_sample(h.sample_format);
    if (bps == 0) { out.error = "Unsupported sampleFormat"; return out; }

    const size_t channel_bytes = static_cast<size_t>(h.width) *
                                 static_cast<size_t>(h.height) * bps;
    if (h.pixel_offset + channel_bytes > size) {
        out.error = "Buffer truncated before pixel data end (need full file)";
        return out;
    }

    // V1: load only the first channel (R for RGB, full data for Gray) and
    // expose it as a single-plane FitsImage. RGB compositing comes later.
    const auto* pix = static_cast<const uint8_t*>(buffer) + h.pixel_offset;
    const size_t npix = static_cast<size_t>(h.width) * static_cast<size_t>(h.height);

    FitsImage img;
    img.width = h.width;
    img.height = h.height;
    img.source_type = h.sample_format;
    img.data.resize(npix);

    auto store = [&](float v, size_t i) {
        // XISF top-down row order matches our convention (no flip needed,
        // unlike FITS).
        img.data[i] = v;
    };

    float vmin = std::numeric_limits<float>::infinity();
    float vmax = -std::numeric_limits<float>::infinity();
    auto track = [&](float v) {
        if (std::isfinite(v)) {
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
    };

    switch (h.sample_format) {
        case PixelType::Float32: {
            const float* src = reinterpret_cast<const float*>(pix);
            for (size_t i = 0; i < npix; ++i) { float v = src[i]; store(v, i); track(v); }
            break;
        }
        case PixelType::Float64: {
            const double* src = reinterpret_cast<const double*>(pix);
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(src[i]); store(v, i); track(v);
            }
            break;
        }
        case PixelType::UInt8: {
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(pix[i]); store(v, i); track(v);
            }
            break;
        }
        case PixelType::UInt16: {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(pix);
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(src[i]); store(v, i); track(v);
            }
            break;
        }
        case PixelType::Int16: {
            const int16_t* src = reinterpret_cast<const int16_t*>(pix);
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(src[i]); store(v, i); track(v);
            }
            break;
        }
        case PixelType::UInt32: {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(pix);
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(src[i]); store(v, i); track(v);
            }
            break;
        }
        case PixelType::Int32: {
            const int32_t* src = reinterpret_cast<const int32_t*>(pix);
            for (size_t i = 0; i < npix; ++i) {
                float v = static_cast<float>(src[i]); store(v, i); track(v);
            }
            break;
        }
        default:
            out.error = "Unsupported sample format";
            return out;
    }
    if (!std::isfinite(vmin) || vmax <= vmin) { vmin = 0.0f; vmax = 1.0f; }
    img.source_min = vmin;
    img.source_max = vmax;
    img.headers = h.fits_keywords;

    out.image = std::move(img);
    out.success = true;
    return out;
}

}  // namespace fitsx
