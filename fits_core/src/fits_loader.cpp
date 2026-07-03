#include "fits_core/fits_loader.h"
#include "fits_core/fits_headers.h"
#include "fits_core/fits_debayer.h"
#include "fits_core/xisf_loader.h"
#include "fits_core/raw_loader.h"
#include "fits_core/image_limits.h"

#include <fitsio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace fitsx {

namespace {

PixelType bitpix_to_type(int bitpix) noexcept {
    switch (bitpix) {
        case BYTE_IMG:    return PixelType::UInt8;
        case SHORT_IMG:   return PixelType::Int16;
        case USHORT_IMG:  return PixelType::UInt16;
        case LONG_IMG:    return PixelType::Int32;
        case ULONG_IMG:   return PixelType::UInt32;
        case FLOAT_IMG:   return PixelType::Float32;
        case DOUBLE_IMG:  return PixelType::Float64;
        default:          return PixelType::Unknown;
    }
}

std::string format_status(int status) {
    char buf[FLEN_ERRMSG] = {0};
    fits_get_errstatus(status, buf);
    return std::string(buf);
}

void enumerate_headers(fitsfile* fptr, std::vector<FitsHeader>& out) {
    int nkeys = 0, status = 0;
    if (fits_get_hdrspace(fptr, &nkeys, nullptr, &status) != 0) return;
    out.reserve(static_cast<size_t>(nkeys));
    for (int i = 1; i <= nkeys; ++i) {
        char keyname[FLEN_KEYWORD] = {0};
        char value[FLEN_VALUE] = {0};
        char comment[FLEN_COMMENT] = {0};
        status = 0;
        if (fits_read_keyn(fptr, i, keyname, value, comment, &status) != 0) continue;
        if (keyname[0] == '\0') continue;
        FitsHeader h;
        h.key = keyname;
        h.value = trim_fits_value(value);
        h.comment = comment;
        out.push_back(std::move(h));
    }
}

LoadResult finish_with_error(fitsfile* fptr, int status, const char* prefix) {
    LoadResult res;
    res.success = false;
    res.error = std::string(prefix ? prefix : "") + format_status(status);
    if (fptr) {
        int s = 0;
        fits_close_file(fptr, &s);
    }
    return res;
}

LoadResult load_from_fitsfile(fitsfile* fptr) {
    LoadResult res;
    int status = 0;

    int hdunum = 0, hdutype = 0;
    fits_get_num_hdus(fptr, &hdunum, &status);
    if (status != 0) return finish_with_error(fptr, status, "fits_get_num_hdus: ");

    int found = 0;
    for (int i = 1; i <= hdunum; ++i) {
        status = 0;
        fits_movabs_hdu(fptr, i, &hdutype, &status);
        if (status == 0 && hdutype == IMAGE_HDU) {
            int naxis = 0;
            status = 0;
            fits_get_img_dim(fptr, &naxis, &status);
            if (status == 0 && naxis >= 2) { found = i; break; }
        }
    }
    if (!found) {
        res.error = "No image HDU found in FITS file";
        int s = 0; fits_close_file(fptr, &s);
        return res;
    }

    int bitpix = 0, naxis = 0;
    long naxes[3] = {0, 0, 0};
    status = 0;
    fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status);
    if (status != 0) return finish_with_error(fptr, status, "fits_get_img_param: ");
    if (naxis < 2 || !dimensions_ok(naxes[0], naxes[1])) {
        res.error = "Image has invalid or excessive dimensions";
        int s = 0; fits_close_file(fptr, &s);
        return res;
    }

    const long width = naxes[0];
    const long height = naxes[1];
    // A NAXIS=3 cube with a 3rd axis of exactly 3 is an already-separated RGB
    // image (planar: R plane, then G, then B). Anything else 3-D is treated as
    // mono (first plane only) -- multi-frame cubes are out of scope.
    const long nplanes = (naxis >= 3 && naxes[2] > 0) ? naxes[2] : 1;
    const bool color_cube = (nplanes == 3);

    double bzero = 0.0, bscale = 1.0;
    {
        int s = 0; double tmp = 0;
        if (fits_read_key(fptr, TDOUBLE, "BZERO", &tmp, nullptr, &s) == 0) bzero = tmp;
        s = 0;
        if (fits_read_key(fptr, TDOUBLE, "BSCALE", &tmp, nullptr, &s) == 0) bscale = tmp;
    }

    // Detect a Bayer CFA on 2-D frames. The pattern letters describe the data
    // in FITS-native (bottom-up) order, so we must demosaic BEFORE flipping
    // rows -- a vertical flip swaps the Bayer row parity (RGGB <-> GBRG).
    BayerPattern bayer = BayerPattern::None;
    int bxoff = 0, byoff = 0;
    if (!color_cube) {
        char bp[FLEN_VALUE] = {0};
        int s = 0;
        if (fits_read_key(fptr, TSTRING, "BAYERPAT", bp, nullptr, &s) == 0) {
            bayer = parse_bayer_pattern(bp);
        }
        if (bayer != BayerPattern::None) {
            int v = 0; s = 0;
            if (fits_read_key(fptr, TINT, "XBAYROFF", &v, nullptr, &s) == 0) bxoff = v;
            else { s = 0; if (fits_read_key(fptr, TINT, "BAYOFFX", &v, nullptr, &s) == 0) bxoff = v; }
            v = 0; s = 0;
            if (fits_read_key(fptr, TINT, "YBAYROFF", &v, nullptr, &s) == 0) byoff = v;
            else { s = 0; if (fits_read_key(fptr, TINT, "BAYOFFY", &v, nullptr, &s) == 0) byoff = v; }
        }
    }

    const size_t npix = static_cast<size_t>(width) * static_cast<size_t>(height);
    const long planes_to_read = color_cube ? 3 : 1;
    std::vector<float> raw(npix * static_cast<size_t>(planes_to_read));
    long fpixel[3] = {1, 1, 1};
    int anynul = 0;
    float nulval = 0.0f;
    status = 0;
    if (fits_read_pix(fptr, TFLOAT, fpixel,
                      static_cast<LONGLONG>(npix) * planes_to_read,
                      &nulval, raw.data(), &anynul, &status) != 0) {
        return finish_with_error(fptr, status, "fits_read_pix: ");
    }

    // Flip a single native (bottom-up) plane into top-down storage, sanitizing
    // NaN/Inf to 0 and folding global min/max. Downstream hot loops (render /
    // analysis / histogram) rely on the sanitize so they can drop the
    // isfinite branch. For RGB the min/max spans all three channels so the
    // shared render LUT covers the brightest channel.
    float vmin = std::numeric_limits<float>::infinity();
    float vmax = -std::numeric_limits<float>::infinity();
    auto flip_into = [&](const float* native, std::vector<float>& dst) {
        dst.resize(npix);
        for (long y = 0; y < height; ++y) {
            const float* src = native + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width);
            float* d = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
            for (long x = 0; x < width; ++x) {
                float v = src[x];
                if (!std::isfinite(v)) v = 0.0f;
                d[x] = v;
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        }
    };
    // In-place vertical flip: swap row y with row (height-1-y), sanitizing +
    // folding min/max as we go. Lets a single-plane source (mono, or each
    // demosaiced plane) be std::move()d straight into the destination instead
    // of copied into a fresh buffer -- avoids holding two full-frame copies of
    // every plane at once (the OSC path peaked at ~7 full-frame buffers).
    // Bit-identical output to flip_into.
    auto flip_in_place = [&](std::vector<float>& v) {
        for (long y = 0; y < height / 2; ++y) {
            float* a = v.data() + static_cast<size_t>(y) * width;
            float* b = v.data() + static_cast<size_t>(height - 1 - y) * width;
            for (long x = 0; x < width; ++x) {
                float va = a[x], vb = b[x];
                if (!std::isfinite(va)) va = 0.0f;
                if (!std::isfinite(vb)) vb = 0.0f;
                a[x] = vb; b[x] = va;              // swap -> top-down
                if (vb < vmin) vmin = vb; if (vb > vmax) vmax = vb;
                if (va < vmin) vmin = va; if (va > vmax) vmax = va;
            }
        }
        if (height & 1) {                          // middle row stays put
            float* m = v.data() + static_cast<size_t>(height / 2) * width;
            for (long x = 0; x < width; ++x) {
                if (!std::isfinite(m[x])) m[x] = 0.0f;
                if (m[x] < vmin) vmin = m[x]; if (m[x] > vmax) vmax = m[x];
            }
        }
    };

    if (color_cube) {
        // raw holds all three planes contiguously, so it can't be moved apart;
        // copy each out (this NAXIS3=3 path is uncommon).
        flip_into(raw.data(),                 res.image.data);
        flip_into(raw.data() + npix,          res.image.data_g);
        flip_into(raw.data() + 2 * npix,      res.image.data_b);
    } else if (bayer != BayerPattern::None) {
        // Sanitize in native order, demosaic, gray-world balance, then flip each
        // reconstructed plane in place and MOVE it into the image.
        for (float& v : raw) { if (!std::isfinite(v)) v = 0.0f; }
        std::vector<float> rr, gg, bb;
        debayer_bilinear(raw, static_cast<int>(width), static_cast<int>(height),
                         bayer, bxoff, byoff, rr, gg, bb);
        std::vector<float>().swap(raw);    // CFA no longer needed -> free it
        gray_world_balance(rr, gg, bb);
        flip_in_place(rr); flip_in_place(gg); flip_in_place(bb);
        res.image.data   = std::move(rr);
        res.image.data_g = std::move(gg);
        res.image.data_b = std::move(bb);
    } else {
        flip_in_place(raw);                // mono: flip the CFA-free single plane
        res.image.data = std::move(raw);   // and hand the buffer over directly
    }
    if (!std::isfinite(vmin) || vmax <= vmin) { vmin = 0.0f; vmax = 1.0f; }

    res.image.width = static_cast<int>(width);
    res.image.height = static_cast<int>(height);
    res.image.source_type = bitpix_to_type(bitpix);
    res.image.source_min = vmin;
    res.image.source_max = vmax;
    res.image.bzero = bzero;
    res.image.bscale = bscale;

    enumerate_headers(fptr, res.image.headers);

    int s = 0; fits_close_file(fptr, &s);
    res.success = true;
    return res;
}

}  // namespace

ImageFormat detect_format(const void* buffer, size_t size) noexcept {
    if (is_xisf(buffer, size)) return ImageFormat::Xisf;
    if (is_raw(buffer, size))  return ImageFormat::Raw;
    return ImageFormat::Fits;
}

FitsMetadata parse_fits_metadata(const void* buffer, size_t size) {
    FitsMetadata md;
    if (!buffer || size == 0) { md.error = "Empty buffer"; return md; }

    fitsfile* fptr = nullptr;
    int status = 0;
    void* mem = const_cast<void*>(buffer);
    size_t mem_size = size;
    if (fits_open_memfile(&fptr, "buffer.fits", READONLY,
                          &mem, &mem_size, 0, nullptr, &status) != 0) {
        md.error = "fits_open_memfile: " + format_status(status);
        return md;
    }

    int hdunum = 0;
    fits_get_num_hdus(fptr, &hdunum, &status);
    int found = 0;
    for (int i = 1; i <= hdunum; ++i) {
        status = 0;
        int hdutype = 0;
        fits_movabs_hdu(fptr, i, &hdutype, &status);
        if (status == 0 && hdutype == IMAGE_HDU) {
            int naxis = 0;
            status = 0;
            fits_get_img_dim(fptr, &naxis, &status);
            if (status == 0 && naxis >= 2) { found = i; break; }
        }
    }
    if (!found) {
        md.error = "No image HDU found in FITS file";
        int s = 0; fits_close_file(fptr, &s);
        return md;
    }

    int bitpix = 0, naxis = 0;
    long naxes[3] = {0, 0, 0};
    status = 0;
    // Reads only the NAXIS* header cards -- no pixel access.
    fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status);
    if (status == 0 && naxis >= 2 && naxes[0] > 0 && naxes[1] > 0) {
        md.width  = static_cast<int>(naxes[0]);
        md.height = static_cast<int>(naxes[1]);
    }
    enumerate_headers(fptr, md.headers);   // keyword cards of the image HDU
    md.success = true;

    int s = 0; fits_close_file(fptr, &s);
    return md;
}

LoadResult load_from_memory(const void* buffer, size_t size, bool prefer_fast) {
    LoadResult res;
    if (!buffer || size == 0) {
        res.error = "Empty buffer";
        return res;
    }

    // Auto-dispatch by magic (see detect_format): XISF starts with "XISF0100",
    // camera RAW (NEF/CR2/ARW/DNG/...) opens with a TIFF byte-order marker, and
    // FITS starts with the ASCII "SIMPLE" card -- no clash, CFITSIO is the
    // final fallback.
    switch (detect_format(buffer, size)) {
        case ImageFormat::Xisf: return load_xisf_from_memory(buffer, size);
        case ImageFormat::Raw:  return load_raw_from_memory(buffer, size, prefer_fast);
        case ImageFormat::Fits: break;  // handled by CFITSIO below (prefer_fast n/a yet)
    }

    fitsfile* fptr = nullptr;
    int status = 0;

    void* mem = const_cast<void*>(buffer);
    size_t mem_size = size;

    if (fits_open_memfile(&fptr, "buffer.fits", READONLY,
                          &mem, &mem_size, 0, nullptr, &status) != 0) {
        res.error = "fits_open_memfile: " + format_status(status);
        return res;
    }
    return load_from_fitsfile(fptr);
}

LoadResult load_from_file(const wchar_t* utf16_path) {
    LoadResult res;
    if (!utf16_path) { res.error = "Null path"; return res; }

    FILE* f = nullptr;
    if (_wfopen_s(&f, utf16_path, L"rb") != 0 || !f) {
        res.error = "Failed to open file";
        return res;
    }
    // 64-bit size: std::ftell returns a 32-bit long on Windows, so a file >= 2 GB
    // (real for stacked masters / large mosaics) would wrap negative and be
    // rejected as "Empty file". Use _fseeki64/_ftelli64.
    if (_fseeki64(f, 0, SEEK_END) != 0) { std::fclose(f); res.error = "fseek failed"; return res; }
    const long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); res.error = "Empty file"; return res; }

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) { res.error = "Truncated read"; return res; }

    return load_from_memory(buf.data(), buf.size());
}

}  // namespace fitsx
