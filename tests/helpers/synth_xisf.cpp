#include "synth_xisf.h"

#include <cstring>
#include <sstream>

namespace wst {

namespace {

void append_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>( v        & 0xFF));
    out.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

}  // namespace

std::vector<uint8_t> make_synth_xisf(int width, int height,
                                     const std::vector<float>& pixels,
                                     const std::string& fits_keyword_name,
                                     const std::string& fits_keyword_value) {
    if (width <= 0 || height <= 0) return {};
    if (static_cast<size_t>(width) * height != pixels.size()) return {};

    // Pixel block starts right after the 16-byte prefix + the XML header.
    // The location offset has to be filled in after we know the XML length —
    // pixel_offset = 16 + xml_length. We build the XML once, measure it,
    // then commit.
    constexpr size_t kPrefix = 16;
    const size_t pixel_bytes = pixels.size() * sizeof(float);

    // Compose XML, leaving "%PIXEL_OFFSET%" as a placeholder of fixed width
    // so the byte count stays predictable (10 digits zero-padded covers up to
    // 9_999_999_999 -> ~9.3 GB, well past any realistic in-memory test).
    constexpr int kOffsetDigits = 10;
    constexpr char kOffsetPlaceholder[kOffsetDigits + 1] = "0000000000";

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\">"
        << "<Image geometry=\"" << width << ":" << height << ":1\""
        <<       " sampleFormat=\"Float32\""
        <<       " colorSpace=\"Gray\""
        <<       " location=\"attachment:" << kOffsetPlaceholder
        <<                            ":" << pixel_bytes << "\">";
    if (!fits_keyword_name.empty()) {
        xml << "<FITSKeyword name=\"" << fits_keyword_name
            <<           "\" value=\""<< fits_keyword_value
            <<           "\" comment=\"\"/>";
    }
    xml << "</Image></xisf>";
    std::string xml_str = xml.str();

    // Now patch the placeholder with the real offset (kPrefix + xml length).
    const uint64_t pixel_offset = kPrefix + xml_str.size();
    char real_offset[kOffsetDigits + 1];
    std::snprintf(real_offset, sizeof(real_offset), "%010llu",
                  static_cast<unsigned long long>(pixel_offset));
    const size_t pos = xml_str.find(kOffsetPlaceholder);
    if (pos == std::string::npos) return {};
    std::memcpy(&xml_str[pos], real_offset, kOffsetDigits);

    // Assemble the byte buffer.
    std::vector<uint8_t> out;
    out.reserve(kPrefix + xml_str.size() + pixel_bytes);
    out.insert(out.end(), {'X','I','S','F','0','1','0','0'});
    append_u32_le(out, static_cast<uint32_t>(xml_str.size()));
    append_u32_le(out, 0);   // reserved
    out.insert(out.end(), xml_str.begin(), xml_str.end());

    const auto* pix_bytes = reinterpret_cast<const uint8_t*>(pixels.data());
    out.insert(out.end(), pix_bytes, pix_bytes + pixel_bytes);
    return out;
}

std::vector<uint8_t> make_synth_xisf_rgb(int width, int height,
                                         const std::vector<float>& r,
                                         const std::vector<float>& g,
                                         const std::vector<float>& b) {
    if (width <= 0 || height <= 0) return {};
    const size_t npix = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (r.size() != npix || g.size() != npix || b.size() != npix) return {};

    constexpr size_t kPrefix = 16;
    const size_t channel_bytes = npix * sizeof(float);
    const size_t pixel_bytes   = 3 * channel_bytes;

    constexpr int  kOffsetDigits = 10;
    constexpr char kOffsetPlaceholder[kOffsetDigits + 1] = "0000000000";

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\">"
        << "<Image geometry=\"" << width << ":" << height << ":3\""
        <<       " sampleFormat=\"Float32\""
        <<       " colorSpace=\"RGB\""
        <<       " location=\"attachment:" << kOffsetPlaceholder
        <<                            ":" << pixel_bytes << "\">"
        << "</Image></xisf>";
    std::string xml_str = xml.str();

    const uint64_t pixel_offset = kPrefix + xml_str.size();
    char real_offset[kOffsetDigits + 1];
    std::snprintf(real_offset, sizeof(real_offset), "%010llu",
                  static_cast<unsigned long long>(pixel_offset));
    const size_t pos = xml_str.find(kOffsetPlaceholder);
    if (pos == std::string::npos) return {};
    std::memcpy(&xml_str[pos], real_offset, kOffsetDigits);

    std::vector<uint8_t> out;
    out.reserve(kPrefix + xml_str.size() + pixel_bytes);
    out.insert(out.end(), {'X','I','S','F','0','1','0','0'});
    append_u32_le(out, static_cast<uint32_t>(xml_str.size()));
    append_u32_le(out, 0);
    out.insert(out.end(), xml_str.begin(), xml_str.end());

    auto append_plane = [&](const std::vector<float>& plane) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(plane.data());
        out.insert(out.end(), bytes, bytes + channel_bytes);
    };
    append_plane(r);
    append_plane(g);
    append_plane(b);
    return out;
}

}  // namespace wst
