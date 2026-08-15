#include "fits_core/fits_writer.h"

#include "fits_core/export_pixels.h"
#include "fits_core/fits_debayer.h"
#include "fits_core/image_rotate.h"

#include <windows.h>

#include <fitsio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fitsx {

namespace {

// ------------------------------------------------------------- utilities ---

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

std::string cfitsio_error(int status) {
    char buf[FLEN_ERRMSG] = {0};
    fits_get_errstatus(status, buf);
    return std::string(buf);
}

// CFITSIO opens files with fopen and a narrow path, so a wide path has to
// survive a round trip through the ANSI code page or it cannot be used at all.
// Returns false when any character would be substituted.
bool ansi_path(const std::wstring& w, std::string& out) {
    if (w.empty()) return false;
    BOOL used_default = FALSE;
    const int n = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                        w.c_str(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, &used_default);
    if (n <= 0 || used_default) return false;
    std::string narrow(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                          w.c_str(), static_cast<int>(w.size()),
                          narrow.data(), n, nullptr, &used_default);
    if (used_default) return false;

    // Round-trip: a code page can map two different wide chars onto one byte
    // without setting used_default.
    const int back = ::MultiByteToWideChar(CP_ACP, 0, narrow.data(), n, nullptr, 0);
    if (back <= 0) return false;
    std::wstring wide_again(static_cast<size_t>(back), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, narrow.data(), n, wide_again.data(), back);
    if (wide_again != w) return false;

    out = std::move(narrow);
    return true;
}

std::wstring dir_of(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
}

// Temp name in the DESTINATION directory, so the rename stays on one volume and
// is therefore atomic. The pid+tick suffix keeps two instances exporting into
// the same folder from deleting each other's temp file.
std::wstring temp_sibling(const std::wstring& dest) {
    wchar_t suffix[64];
    ::swprintf_s(suffix, L".winstellar-tmp-%lu-%llu",
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long long>(::GetTickCount64()));
    return dest + suffix;
}

// Owns the buffer CFITSIO's memkeep driver hands back -- fits_close_file does
// NOT free it, on any path including the error ones.
struct MemBuf {
    void* p = nullptr;
    ~MemBuf() { if (p) std::free(p); }
};

bool write_all(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t left = size;
    bool ok = true;
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 4u << 20));
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) { ok = false; break; }
        p += wrote;
        left -= wrote;
    }
    ::CloseHandle(h);
    return ok;
}

bool cancelled(const std::function<bool()>& f) { return f && f(); }

// ---------------------------------------------------------- header policy ---

// CFITSIO writes these itself; copying them contradicts the array it just wrote
// and produces a file that is structurally invalid or lies about its own data.
bool is_structural(const std::string& K) {
    static const char* kBlocked[] = {
        "SIMPLE", "XTENSION", "BITPIX", "NAXIS", "EXTEND", "PCOUNT", "GCOUNT",
        "TFIELDS", "BZERO", "BSCALE", "BLANK", "DATAMIN", "DATAMAX",
        "CHECKSUM", "DATASUM", "DATE", "END", "CONTINUE", "ROWORDER",
    };
    for (const char* b : kBlocked) if (K == b) return true;
    // NAXIS1, NAXIS2, NAXIS3...
    if (starts_with(K, "NAXIS") && K.size() > 5 &&
        std::all_of(K.begin() + 5, K.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        return true;
    return false;
}

bool is_cfa_key(const std::string& K) {
    return K == "BAYERPAT" || K == "XBAYROFF" || K == "YBAYROFF" ||
           K == "BAYOFFX"  || K == "BAYOFFY";
}

// Orientation-dependent astrometry. A rotated array invalidates all of it, and
// a stale WCS is worse than none: it silently mis-solves the field.
bool is_orientation_wcs(const std::string& K) {
    static const char* kWcs[] = {
        "WCSAXES", "CTYPE1", "CTYPE2", "CUNIT1", "CUNIT2", "CRPIX1", "CRPIX2",
        "CRVAL1", "CRVAL2", "CDELT1", "CDELT2", "CROTA1", "CROTA2",
        "CD1_1", "CD1_2", "CD2_1", "CD2_2", "PC1_1", "PC1_2", "PC2_1", "PC2_2",
        "LONPOLE", "LATPOLE", "IMAGEW", "IMAGEH",
        "A_ORDER", "B_ORDER", "AP_ORDER", "BP_ORDER",
    };
    for (const char* w : kWcs) if (K == w) return true;
    // SIP distortion terms.
    if (starts_with(K, "A_") || starts_with(K, "B_") ||
        starts_with(K, "AP_") || starts_with(K, "BP_")) return true;
    return false;
}

// Keywords whose value is textual even when it looks numeric. OBJECT = 7331 is
// NGC 7331, not the integer 7331; writing it as a number loses that.
bool always_string(const std::string& K) {
    static const char* kStr[] = {
        "OBJECT", "IMAGETYP", "FILTER", "TELESCOP", "INSTRUME", "OBSERVER",
        "DATE-OBS", "DATE-END", "BAYERPAT", "SWCREATE", "SWOWNER",
        "OBJCTRA", "OBJCTDEC", "SITELAT", "SITELONG", "FOCNAME", "CAMERA",
        "ORIGIN", "NOTES",
    };
    for (const char* s : kStr) if (K == s) return true;
    return false;
}

bool parse_ll(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

bool parse_double_fits(const std::string& s, double& out) {
    if (s.empty()) return false;
    // FITS allows a 'D' exponent (1.5D3); strtod does not.
    std::string t = s;
    for (char& c : t) if (c == 'D' || c == 'd') c = 'E';
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(t.c_str(), &end);
    if (errno != 0 || end != t.c_str() + t.size()) return false;
    out = v;
    return true;
}

void write_history(fitsfile* fp, const std::string& text, int* status) {
    fits_write_history(fp, const_cast<char*>(text.c_str()), status);
}

// Copy the source keywords onto a freshly created image HDU.
void copy_headers(fitsfile* fp, const FitsImage& img, bool out_is_rgb,
                  bool rotated, int* status) {
    for (const auto& h : img.headers) {
        if (*status != 0) return;
        const std::string K = upper(h.key);
        if (K.empty() || is_structural(K)) continue;
        if (out_is_rgb && is_cfa_key(K)) continue;      // never re-demosaic RGB
        if (rotated && is_orientation_wcs(K)) continue;
        if (K == "COMMENT") {
            if (!h.comment.empty())
                fits_write_comment(fp, const_cast<char*>(h.comment.c_str()), status);
            continue;
        }
        if (K == "HISTORY") {
            if (!h.comment.empty())
                fits_write_history(fp, const_cast<char*>(h.comment.c_str()), status);
            continue;
        }
        if (is_cfa_key(K)) continue;   // rewritten explicitly below, never copied

        const char* key = h.key.c_str();
        const char* cmt = h.comment.empty() ? nullptr : h.comment.c_str();
        const std::string V = upper(h.value);

        if (!always_string(K) && (V == "T" || V == "F")) {
            int b = (V == "T") ? 1 : 0;
            fits_update_key(fp, TLOGICAL, const_cast<char*>(key), &b,
                            const_cast<char*>(cmt), status);
            continue;
        }
        long long iv = 0;
        double dv = 0.0;
        if (!always_string(K) && parse_ll(h.value, iv)) {
            fits_update_key(fp, TLONGLONG, const_cast<char*>(key), &iv,
                            const_cast<char*>(cmt), status);
        } else if (!always_string(K) && parse_double_fits(h.value, dv)) {
            fits_update_key(fp, TDOUBLE, const_cast<char*>(key), &dv,
                            const_cast<char*>(cmt), status);
        } else {
            fits_update_key(fp, TSTRING, const_cast<char*>(key),
                            const_cast<char*>(h.value.c_str()),
                            const_cast<char*>(cmt), status);
        }
    }
}

// ----------------------------------------------------------------- sinks ---

// Everything below writes through one of two sinks. Tier 1 hands CFITSIO a
// narrow path and lets it stream to disk. Tier 2 builds the file in memory and
// writes it out with a wide-path handle, for destinations the ANSI code page
// cannot express.
struct Sink {
    bool        memfile = false;
    std::string narrow;       // tier 1
    MemBuf      buf;          // tier 2
    size_t      buf_size = 0;
};

bool open_sink(Sink& sink, const std::wstring& temp_path, bool force_memfile,
               fitsfile** fp, int* status) {
    if (!force_memfile && ansi_path(temp_path, sink.narrow)) {
        sink.memfile = false;
        return fits_create_file(fp, sink.narrow.c_str(), status) == 0;
    }
    sink.memfile = true;
    sink.buf_size = 64u << 10;
    sink.buf.p = std::malloc(sink.buf_size);
    if (!sink.buf.p) return false;
    // 16 MB growth, not the 1 MB default: reallocating a large block copies it,
    // so a small delta turns a 500 MB export into tens of GB of memcpy.
    return fits_create_memfile(fp, &sink.buf.p, &sink.buf_size,
                               16u << 20, std::realloc, status) == 0;
}

// Close the CFITSIO handle and, for tier 2, flush the bytes to the wide path.
// The memfile buffer is ALLOCATED size, not file length -- writing all of it
// appends uninitialised heap past the last 2880-byte block.
bool close_sink(Sink& sink, fitsfile* fp, const std::wstring& temp_path, int* status) {
    if (!sink.memfile) {
        fits_close_file(fp, status);
        return *status == 0;
    }
    LONGLONG head = 0, data = 0, dataend = 0;
    int nhdus = 0;
    fits_flush_file(fp, status);
    fits_get_num_hdus(fp, &nhdus, status);
    fits_movabs_hdu(fp, nhdus, nullptr, status);
    fits_get_hduaddrll(fp, &head, &data, &dataend, status);
    const size_t length = static_cast<size_t>(dataend);
    fits_close_file(fp, status);
    if (*status != 0 || length == 0 || length > sink.buf_size) return false;
    return write_all(temp_path, sink.buf.p, length);
}

}  // namespace

// ------------------------------------------------------------- the probe ---

namespace {

// The HDU the display loader picked: the first IMAGE_HDU with NAXIS >= 2.
// KEEP IN STEP with load_from_fitsfile in fits_loader.cpp -- if the two ever
// disagree, the export silently writes a different HDU than the one on screen.
int select_image_hdu(fitsfile* fp, int* status) {
    int nhdu = 0, hdutype = 0;
    fits_get_num_hdus(fp, &nhdu, status);
    if (*status != 0) return 0;
    for (int i = 1; i <= nhdu; ++i) {
        int s = 0;
        fits_movabs_hdu(fp, i, &hdutype, &s);
        if (s == 0 && hdutype == IMAGE_HDU) {
            int naxis = 0;
            s = 0;
            fits_get_img_dim(fp, &naxis, &s);
            if (s == 0 && naxis >= 2) return i;
        }
    }
    return 0;
}

std::string read_key_str(fitsfile* fp, const char* key) {
    char val[FLEN_VALUE] = {0};
    int s = 0;
    if (fits_read_key(fp, TSTRING, const_cast<char*>(key), val, nullptr, &s) != 0) return {};
    return std::string(val);
}

FitsSourceStatus inspect_open_source(fitsfile* fp, int expect_width, int expect_height,
                                     int display_rotation_deg, int* hdu_out) {
    int status = 0;
    const int hdu = select_image_hdu(fp, &status);
    if (hdu == 0) return FitsSourceStatus::NotFits;
    if (hdu_out) *hdu_out = hdu;

    // A tile-compressed HDU also reports as IMAGE_HDU, which is why the loader
    // opens .fz at all; re-encoding one through resize/write generates garbage.
    if (fits_is_compressed_image(fp, &status) != 0 || status != 0)
        return status != 0 ? FitsSourceStatus::Unreadable : FitsSourceStatus::CompressedOrCube;

    int naxis = 0;
    long naxes[3] = {0, 0, 0};
    fits_get_img_dim(fp, &naxis, &status);
    fits_get_img_size(fp, naxis > 3 ? 3 : naxis, naxes, &status);
    if (status != 0) return FitsSourceStatus::Unreadable;
    if (naxis > 2 && naxes[2] != 3) return FitsSourceStatus::CompressedOrCube;
    if (expect_width > 0 && expect_height > 0 &&
        (naxes[0] != expect_width || naxes[1] != expect_height))
        return FitsSourceStatus::Changed;

    if (display_rotation_deg % 360 != 0) {
        // Rotating a mosaic we cannot name would ship a file that decodes with
        // swapped colours -- silently. Refuse instead.
        const std::string pat = read_key_str(fp, "BAYERPAT");
        if (!pat.empty() && parse_bayer_pattern(pat) == BayerPattern::None)
            return FitsSourceStatus::UnrotatableCfa;

        // The bottom-up conjugation is what makes the rotation come out the way
        // the screen shows it; a file that declares another row order breaks it.
        const std::string ro = upper(read_key_str(fp, "ROWORDER"));
        if (!ro.empty() && ro.find("BOTTOM-UP") == std::string::npos)
            return FitsSourceStatus::UnrotatableRowOrder;
    }
    return FitsSourceStatus::Usable;
}

}  // namespace

FitsSourceStatus probe_fits_source(const std::wstring& path,
                                   int expect_width, int expect_height,
                                   int display_rotation_deg) {
    if (path.empty()) return FitsSourceStatus::NoPath;

    const DWORD attr = ::GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return FitsSourceStatus::Missing;

    std::string narrow;
    if (!ansi_path(path, narrow)) return FitsSourceStatus::Unreadable;

    fitsfile* fp = nullptr;
    int status = 0;
    if (fits_open_diskfile(&fp, narrow.c_str(), READONLY, &status) != 0)
        return FitsSourceStatus::NotFits;

    const FitsSourceStatus out =
        inspect_open_source(fp, expect_width, expect_height, display_rotation_deg, nullptr);
    int close_status = 0;
    fits_close_file(fp, &close_status);
    return out;
}

// ------------------------------------------------------------ the writer ---

namespace {

// Write the in-memory (debayered, white-balanced) image. This is the fallback:
// it is a faithful record of what the viewer holds, but it is NOT the user's
// original mosaic, and the HISTORY says so.
FitsExportResult write_from_image(const std::wstring& temp_path,
                                  const FitsImage& img,
                                  const FitsExportOptions& opt,
                                  const std::function<bool()>& is_cancelled) {
    FitsExportResult res;
    res.path = FitsExportPath::FromImage;

    const int w = img.width, h = img.height;
    const RotMap m = make_rot_map(w, h, opt.display_rotation_deg);
    const bool rotated = (m.deg != 0);
    const bool rgb = img.is_rgb();

    int xoff = 0, yoff = 0;
    const BayerPattern src_pat = detect_bayer_pattern(img, xoff, yoff);
    BayerPattern out_pat = BayerPattern::None;
    if (!rgb && src_pat != BayerPattern::None) {
        out_pat = rotate_bayer_pattern(src_pat, xoff, yoff, w, h, opt.display_rotation_deg);
        if (out_pat == BayerPattern::None) {
            res.error = "The Bayer pattern of this frame cannot be rotated safely.";
            return res;
        }
    }

    Sink sink;
    fitsfile* fp = nullptr;
    int status = 0;
    if (!open_sink(sink, temp_path, opt.force_memfile_sink, &fp, &status)) {
        res.error = "Could not create the export file: " + cfitsio_error(status);
        return res;
    }

    long naxes[3] = { m.out_w, m.out_h, 3 };
    const int naxis = rgb ? 3 : 2;
    if (fits_create_img(fp, FLOAT_IMG, naxis, naxes, &status) != 0) {
        int s2 = 0; fits_close_file(fp, &s2);
        res.error = "Could not create the image: " + cfitsio_error(status);
        return res;
    }

    // One reusable plane buffer. Materialising all three at once would double
    // the peak footprint on a large colour frame for no reason.
    const size_t npix = static_cast<size_t>(m.out_w) * static_cast<size_t>(m.out_h);
    std::vector<float> plane(npix);
    const std::vector<float>* planes[3] = { &img.data, &img.data_g, &img.data_b };

    for (int p = 0; p < (rgb ? 3 : 1); ++p) {
        if (cancelled(is_cancelled)) {
            int s2 = 0; fits_close_file(fp, &s2);
            res.cancelled = true;
            return res;
        }
        const std::vector<float>& src = *planes[p];
        // FitsImage rows are top-down; FITS stores bottom-up. Rotate and flip
        // in the same pass so the geometry is decided in exactly one place.
        for (int fr = 0; fr < m.out_h; ++fr) {
            const int dy = m.out_h - 1 - fr;
            float* row = plane.data() + static_cast<size_t>(fr) * m.out_w;
            for (int x = 0; x < m.out_w; ++x) row[x] = src[m.src(x, dy)];
        }
        long fpixel[3] = { 1, 1, p + 1 };
        if (fits_write_pix(fp, TFLOAT, fpixel, static_cast<LONGLONG>(npix),
                           plane.data(), &status) != 0) {
            int s2 = 0; fits_close_file(fp, &s2);
            res.error = "Could not write pixel data: " + cfitsio_error(status);
            return res;
        }
    }

    copy_headers(fp, img, rgb, rotated, &status);

    if (out_pat != BayerPattern::None) {
        const char* name = bayer_pattern_name(out_pat);
        long zero = 0;
        fits_update_key(fp, TSTRING, const_cast<char*>("BAYERPAT"),
                        const_cast<char*>(name),
                        const_cast<char*>("CFA pattern of this array"), &status);
        fits_update_key(fp, TLONG, const_cast<char*>("XBAYROFF"), &zero,
                        const_cast<char*>("pattern origin folded in"), &status);
        fits_update_key(fp, TLONG, const_cast<char*>("YBAYROFF"), &zero,
                        const_cast<char*>("pattern origin folded in"), &status);
    }

    fits_update_key(fp, TSTRING, const_cast<char*>("ROWORDER"),
                    const_cast<char*>("BOTTOM-UP"),
                    const_cast<char*>("row order of this array"), &status);
    const std::string prog = "WinStellar " + opt.app_version;
    fits_update_key(fp, TSTRING, const_cast<char*>("PROGRAM"),
                    const_cast<char*>(prog.c_str()),
                    const_cast<char*>("file written by"), &status);
    fits_write_date(fp, &status);

    write_history(fp, "WinStellar " + opt.app_version + " export from " +
                      (opt.source_name.empty() ? std::string("(unnamed)") : opt.source_name),
                  &status);
    if (rotated)
        write_history(fp, "rotated " + std::to_string(m.deg) + " deg CW (display orientation)",
                      &status);
    if (out_pat != BayerPattern::None && src_pat != out_pat)
        write_history(fp, std::string("BAYERPAT ") + bayer_pattern_name(src_pat) + " -> " +
                          bayer_pattern_name(out_pat) + "; XBAYROFF/YBAYROFF folded to 0",
                      &status);
    if (rotated)
        write_history(fp, "WCS keywords removed: image was rotated", &status);
    if (rgb && src_pat != BayerPattern::None)
        write_history(fp, "pixels are debayered and gray-world white balanced; "
                          "not the original CFA mosaic", &status);

    fits_write_chksum(fp, &status);

    if (status != 0) {
        int s2 = 0; fits_close_file(fp, &s2);
        res.error = "Could not finish the file: " + cfitsio_error(status);
        return res;
    }
    if (!close_sink(sink, fp, temp_path, &status)) {
        res.error = status != 0 ? ("Could not close the file: " + cfitsio_error(status))
                                : std::string("Could not write the export file.");
        return res;
    }

    res.success = true;
    res.carries_cfa = (out_pat != BayerPattern::None);
    return res;
}

// Re-encode the ORIGINAL array from the source file: original mosaic, original
// BITPIX, original keywords. Only reached when the frame is rotated -- an
// unrotated export is a byte copy, which is strictly more faithful.
FitsExportResult write_from_source(const std::wstring& temp_path,
                                   const FitsExportOptions& opt,
                                   const std::function<bool()>& is_cancelled) {
    FitsExportResult res;
    res.path = FitsExportPath::SourceReencode;

    std::string src_narrow;
    if (!ansi_path(opt.source_path, src_narrow)) {
        res.error = "The source path cannot be opened for re-reading.";
        return res;
    }

    fitsfile* in = nullptr;
    int status = 0;
    if (fits_open_diskfile(&in, src_narrow.c_str(), READONLY, &status) != 0) {
        res.error = "Could not re-open the source file: " + cfitsio_error(status);
        return res;
    }

    int hdu = 0;
    const FitsSourceStatus usable =
        inspect_open_source(in, opt.expect_width, opt.expect_height,
                            opt.display_rotation_deg, &hdu);
    if (usable != FitsSourceStatus::Usable) {
        int s2 = 0; fits_close_file(in, &s2);
        res.error = "The source file can no longer be used for an exact export.";
        return res;
    }

    int bitpix = 0, naxis = 0;
    long naxes[3] = {0, 0, 0};
    fits_get_img_type(in, &bitpix, &status);        // RAW bitpix, not the BZERO-adjusted one
    fits_get_img_dim(in, &naxis, &status);
    fits_get_img_size(in, naxis > 3 ? 3 : naxis, naxes, &status);
    if (status != 0) {
        int s2 = 0; fits_close_file(in, &s2);
        res.error = "Could not read the source geometry: " + cfitsio_error(status);
        return res;
    }
    const int w = static_cast<int>(naxes[0]);
    const int h = static_cast<int>(naxes[1]);
    const int nplanes = (naxis > 2) ? static_cast<int>(naxes[2]) : 1;

    int dtype = 0;
    switch (bitpix) {
        case BYTE_IMG:     dtype = TBYTE;     break;
        case SHORT_IMG:    dtype = TSHORT;    break;
        case LONG_IMG:     dtype = TINT;      break;
        case LONGLONG_IMG: dtype = TLONGLONG; break;
        case FLOAT_IMG:    dtype = TFLOAT;    break;
        case DOUBLE_IMG:   dtype = TDOUBLE;   break;
        default:
            int s2 = 0; fits_close_file(in, &s2);
            res.error = "Unsupported source bit depth.";
            return res;
    }
    const size_t elem = static_cast<size_t>(std::abs(bitpix)) / 8;

    Sink sink;
    fitsfile* out = nullptr;
    if (!open_sink(sink, temp_path, opt.force_memfile_sink, &out, &status)) {
        int s2 = 0; fits_close_file(in, &s2);
        res.error = "Could not create the export file: " + cfitsio_error(status);
        return res;
    }

    // Copy every HDU verbatim. This IS the header policy for this branch: every
    // keyword keeps its true type, COMMENT/HISTORY/CONTINUE/HIERARCH survive,
    // secondary HDUs survive, and the structural cards stay CFITSIO's business.
    fits_copy_file(in, out, 1, 1, 1, &status);
    fits_movabs_hdu(out, hdu, nullptr, &status);
    fits_movabs_hdu(in, hdu, nullptr, &status);
    if (status != 0) {
        int s2 = 0; fits_close_file(in, &s2); fits_close_file(out, &s2);
        res.error = "Could not copy the source structure: " + cfitsio_error(status);
        return res;
    }

    const RotMap m = make_rot_map(w, h, array_rotation_for_display(opt.display_rotation_deg));
    if (m.out_w != w || m.out_h != h) {
        long out_naxes[3] = { m.out_w, m.out_h, naxes[2] };
        fits_resize_img(out, bitpix, naxis, out_naxes, &status);
        if (status != 0) {
            int s2 = 0; fits_close_file(in, &s2); fits_close_file(out, &s2);
            res.error = "Could not resize the exported image: " + cfitsio_error(status);
            return res;
        }
    }

    // Read and write RAW stored values. With the BZERO/BSCALE cards fits_copy_file
    // already carried over, an unsigned-16 frame round-trips bit for bit.
    fits_set_bscale(in, 1.0, 0.0, &status);
    fits_set_bscale(out, 1.0, 0.0, &status);

    const size_t npix = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<uint8_t> buf(npix * elem), rot(npix * elem);
    for (int p = 0; p < nplanes; ++p) {
        if (cancelled(is_cancelled)) {
            int s2 = 0; fits_close_file(in, &s2); fits_close_file(out, &s2);
            res.cancelled = true;
            return res;
        }
        const LONGLONG first = static_cast<LONGLONG>(p) * static_cast<LONGLONG>(npix) + 1;
        int anynul = 0;
        // nulval is deliberately null: the display loader substitutes 0 for
        // BLANK pixels, and an export sold as the original must not.
        fits_read_img(in, dtype, first, static_cast<LONGLONG>(npix), nullptr,
                      buf.data(), &anynul, &status);
        if (status != 0) break;
        rotate_elements(buf.data(), m, elem, rot.data());
        fits_write_img(out, dtype, first, static_cast<LONGLONG>(npix), rot.data(), &status);
        if (status != 0) break;
    }
    if (status != 0) {
        int s2 = 0; fits_close_file(in, &s2); fits_close_file(out, &s2);
        res.error = "Could not re-encode the pixel data: " + cfitsio_error(status);
        return res;
    }

    // Bayer pattern must follow its mosaic.
    const std::string pat_s = read_key_str(in, "BAYERPAT");
    const BayerPattern src_pat = pat_s.empty() ? BayerPattern::None
                                               : parse_bayer_pattern(pat_s);
    BayerPattern out_pat = BayerPattern::None;
    if (src_pat != BayerPattern::None) {
        long v = 0;
        int s = 0, xoff = 0, yoff = 0;
        s = 0; if (fits_read_key(in, TLONG, const_cast<char*>("XBAYROFF"), &v, nullptr, &s) == 0) xoff = static_cast<int>(v);
        s = 0; if (fits_read_key(in, TLONG, const_cast<char*>("BAYOFFX"),  &v, nullptr, &s) == 0) xoff = static_cast<int>(v);
        s = 0; if (fits_read_key(in, TLONG, const_cast<char*>("YBAYROFF"), &v, nullptr, &s) == 0) yoff = static_cast<int>(v);
        s = 0; if (fits_read_key(in, TLONG, const_cast<char*>("BAYOFFY"),  &v, nullptr, &s) == 0) yoff = static_cast<int>(v);

        out_pat = rotate_bayer_pattern(src_pat, xoff, yoff, w, h, opt.display_rotation_deg);
        if (out_pat == BayerPattern::None) {
            int s2 = 0; fits_close_file(in, &s2); fits_close_file(out, &s2);
            res.error = "The Bayer pattern of this frame cannot be rotated safely.";
            return res;
        }
        const char* name = bayer_pattern_name(out_pat);
        fits_update_key(out, TSTRING, const_cast<char*>("BAYERPAT"),
                        const_cast<char*>(name),
                        const_cast<char*>("CFA pattern of this array"), &status);
        for (const char* k : {"XBAYROFF", "YBAYROFF", "BAYOFFX", "BAYOFFY"}) {
            int s = 0;
            fits_delete_key(out, const_cast<char*>(k), &s);   // KEY_NO_EXIST is fine
        }
    }
    int close_in = 0;
    fits_close_file(in, &close_in);

    // A rotated array invalidates the astrometry; a stale solution mis-solves
    // the field silently, which is worse than having none.
    {
        int nkeys = 0, s = 0;
        std::vector<std::string> doomed;
        fits_get_hdrspace(out, &nkeys, nullptr, &s);
        for (int i = 1; i <= nkeys && s == 0; ++i) {
            char keyname[FLEN_KEYWORD] = {0}, value[FLEN_VALUE] = {0}, comment[FLEN_COMMENT] = {0};
            int s2 = 0;
            if (fits_read_keyn(out, i, keyname, value, comment, &s2) != 0) continue;
            const std::string K = upper(keyname);
            if (is_orientation_wcs(K) || K == "CHECKSUM" || K == "DATASUM")
                doomed.push_back(keyname);
        }
        for (const auto& k : doomed) {
            int s2 = 0;
            fits_delete_key(out, const_cast<char*>(k.c_str()), &s2);
        }
    }

    const std::string prog = "WinStellar " + opt.app_version;
    fits_update_key(out, TSTRING, const_cast<char*>("PROGRAM"),
                    const_cast<char*>(prog.c_str()),
                    const_cast<char*>("file written by"), &status);
    fits_write_date(out, &status);
    write_history(out, "WinStellar " + opt.app_version + " export from " +
                       (opt.source_name.empty() ? std::string("(unnamed)") : opt.source_name),
                  &status);
    write_history(out, "rotated " + std::to_string(opt.display_rotation_deg) +
                       " deg CW (display orientation)", &status);
    if (out_pat != BayerPattern::None && out_pat != src_pat)
        write_history(out, std::string("BAYERPAT ") + bayer_pattern_name(src_pat) + " -> " +
                           bayer_pattern_name(out_pat) + "; XBAYROFF/YBAYROFF folded to 0",
                      &status);
    write_history(out, "WCS keywords removed: image was rotated", &status);
    fits_write_chksum(out, &status);

    if (status != 0) {
        int s2 = 0; fits_close_file(out, &s2);
        res.error = "Could not finish the file: " + cfitsio_error(status);
        return res;
    }
    if (!close_sink(sink, out, temp_path, &status)) {
        res.error = status != 0 ? ("Could not close the file: " + cfitsio_error(status))
                                : std::string("Could not write the export file.");
        return res;
    }
    res.success = true;
    res.carries_cfa = (src_pat != BayerPattern::None);
    return res;
}

}  // namespace

FitsExportResult write_fits_export(const std::wstring& dest_path,
                                   const FitsImage& img,
                                   const FitsExportOptions& opt,
                                   const std::function<bool()>& is_cancelled) {
    FitsExportResult res;
    if (dest_path.empty()) { res.error = "No destination path."; return res; }
    if (img.empty() && opt.mode == FitsExportMode::FromImageOnly) {
        res.error = "No image is loaded.";
        return res;
    }

    const std::wstring temp = temp_sibling(dest_path);

    // Prefer the source file. The in-memory image has been debayered and white
    // balanced and the CFA buffer freed, so it is no longer the user's mosaic.
    bool use_source = (opt.mode == FitsExportMode::FromSourceOnly);
    if (opt.mode == FitsExportMode::Auto)
        use_source = probe_fits_source(opt.source_path, opt.expect_width, opt.expect_height,
                                       opt.display_rotation_deg) == FitsSourceStatus::Usable;

    if (use_source && opt.display_rotation_deg % 360 == 0) {
        // Nothing is more faithful than the original bytes, and it is instant
        // even on a 700 MB master.
        if (::CopyFileW(opt.source_path.c_str(), temp.c_str(), TRUE)) {
            if (::MoveFileExW(temp.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                res.success = true;
                res.path = FitsExportPath::VerbatimCopy;
                res.carries_cfa = true;
                return res;
            }
        }
        ::DeleteFileW(temp.c_str());
        if (opt.mode == FitsExportMode::FromSourceOnly) {
            res.error = "Could not copy the source file to the destination.";
            return res;
        }
        use_source = false;   // fall through to a re-encode / the image
    }

    if (use_source) {
        res = write_from_source(temp, opt, is_cancelled);
        if (!res.success && !res.cancelled && opt.mode == FitsExportMode::Auto) {
            ::DeleteFileW(temp.c_str());
            res = write_from_image(temp, img, opt, is_cancelled);
        }
    } else if (opt.mode == FitsExportMode::FromSourceOnly) {
        res.error = "The original file is not available for an exact export.";
        return res;
    } else {
        res = write_from_image(temp, img, opt, is_cancelled);
    }

    if (!res.success) {
        ::DeleteFileW(temp.c_str());
        return res;
    }
    if (!::MoveFileExW(temp.c_str(), dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temp.c_str());
        res.success = false;
        res.error = "Could not replace the destination file.";
        return res;
    }
    return res;
}

}  // namespace fitsx
