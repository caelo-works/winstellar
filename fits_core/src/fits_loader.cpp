#include "fits_core/fits_loader.h"
#include "fits_core/fits_headers.h"
#include "fits_core/xisf_loader.h"

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
    if (naxis < 2 || naxes[0] <= 0 || naxes[1] <= 0) {
        res.error = "Image has invalid dimensions";
        int s = 0; fits_close_file(fptr, &s);
        return res;
    }

    const long width = naxes[0];
    const long height = naxes[1];

    double bzero = 0.0, bscale = 1.0;
    {
        int s = 0; double tmp = 0;
        if (fits_read_key(fptr, TDOUBLE, "BZERO", &tmp, nullptr, &s) == 0) bzero = tmp;
        s = 0;
        if (fits_read_key(fptr, TDOUBLE, "BSCALE", &tmp, nullptr, &s) == 0) bscale = tmp;
    }

    const size_t npix = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> raw(npix);
    long fpixel[3] = {1, 1, 1};
    int anynul = 0;
    float nulval = 0.0f;
    status = 0;
    if (fits_read_pix(fptr, TFLOAT, fpixel, static_cast<long>(npix),
                      &nulval, raw.data(), &anynul, &status) != 0) {
        return finish_with_error(fptr, status, "fits_read_pix: ");
    }

    // FITS row 1 == bottom of image; flip to top-down to match Windows bitmap order.
    std::vector<float> flipped(npix);
    for (long y = 0; y < height; ++y) {
        const float* src = raw.data() + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width);
        float* dst = flipped.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(width));
    }

    float vmin = std::numeric_limits<float>::infinity();
    float vmax = -std::numeric_limits<float>::infinity();
    for (float v : flipped) {
        if (std::isfinite(v)) {
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
    }
    if (!std::isfinite(vmin) || vmax <= vmin) { vmin = 0.0f; vmax = 1.0f; }

    res.image.width = static_cast<int>(width);
    res.image.height = static_cast<int>(height);
    res.image.source_type = bitpix_to_type(bitpix);
    res.image.data = std::move(flipped);
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

LoadResult load_from_memory(const void* buffer, size_t size) {
    LoadResult res;
    if (!buffer || size == 0) {
        res.error = "Empty buffer";
        return res;
    }

    // Auto-dispatch by magic. XISF (PixInsight) files start with "XISF0100".
    if (is_xisf(buffer, size)) {
        return load_xisf_from_memory(buffer, size);
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
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); res.error = "fseek failed"; return res; }
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); res.error = "Empty file"; return res; }

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) { res.error = "Truncated read"; return res; }

    return load_from_memory(buf.data(), buf.size());
}

}  // namespace fitsx
