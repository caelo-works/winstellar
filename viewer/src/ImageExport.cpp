#include "ImageExport.h"

#include "fits_core/export_pixels.h"
#include "fits_core/fits_render.h"
#include "fits_core/fits_writer.h"
#include "fits_core/image_rotate.h"

#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace wsx {

namespace {

template <typename T>
void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring temp_sibling(const std::wstring& dest) {
    wchar_t suffix[64];
    ::swprintf_s(suffix, L".winstellar-tmp-%lu-%llu",
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long long>(::GetTickCount64()));
    return dest + suffix;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

const GUID& container_for(Format f) {
    switch (f) {
        case Format::Png:  return GUID_ContainerFormatPng;
        case Format::Tiff: return GUID_ContainerFormatTiff;
        default:           return GUID_ContainerFormatJpeg;
    }
}

// Write one metadata string, best-effort: a missing metadata handler must not
// fail an otherwise good export.
void put_meta(IWICMetadataQueryWriter* w, const wchar_t* path, const std::wstring& value) {
    if (!w || value.empty()) return;
    PROPVARIANT v;
    ::PropVariantInit(&v);
    v.vt = VT_LPWSTR;
    v.pwszVal = const_cast<wchar_t*>(value.c_str());
    w->SetMetadataByName(path, &v);
    v.pwszVal = nullptr;          // the string is ours; do not let PropVariantClear free it
    ::PropVariantClear(&v);
}

std::string header_or(const fitsx::FitsImage& img, const char* key, const char* fallback) {
    std::string v;
    if (img.find_header(key, v) && !v.empty()) return v;
    return fallback;
}

// What a viewer of the exported file needs in order to interpret the numbers.
std::wstring linear_description(const fitsx::Linear16Scale& s, int rot,
                                const std::string& version) {
    wchar_t buf[512];
    if (s.identity) {
        ::swprintf_s(buf,
            L"WinStellar %hs linear export. Unstretched. Pixel values are the source "
            L"data verbatim (integers, unrescaled). Rotation: %d deg CW.",
            version.c_str(), rot);
    } else {
        ::swprintf_s(buf,
            L"WinStellar %hs linear export. Unstretched. DN 0..65535 maps linearly to "
            L"source range [%.9g, %.9g]; value = lo + DN*(hi-lo)/65535. Rotation: %d deg CW.",
            version.c_str(), s.lo, s.hi, rot);
    }
    return buf;
}

struct MetaPaths { const wchar_t* software; const wchar_t* description; };

MetaPaths meta_paths_for(Format f) {
    switch (f) {
        case Format::Png:  return { L"/tEXt/{str=Software}", L"/tEXt/{str=Description}" };
        case Format::Tiff: return { L"/ifd/{ushort=305}",     L"/ifd/{ushort=270}" };
        default:           return { L"/app1/ifd/{ushort=305}", L"/app1/ifd/{ushort=270}" };
    }
}

// The WIC half of the job. Rows are produced by `fill`, one block at a time, so
// a large frame never needs a second full copy in memory.
ExportOutcome encode_with_wic(const ExportRequest& req,
                              const fitsx::RotMap& m,
                              const WICPixelFormatGUID& wanted,
                              unsigned bytes_per_pixel,
                              const std::wstring& description,
                              const std::function<void(int, int, void*)>& fill,
                              const std::function<bool()>& is_cancelled) {
    ExportOutcome out;
    out.token = req.token;
    out.dest_path = req.dest_path;

    const std::wstring temp = temp_sibling(req.dest_path);

    IWICImagingFactory*     factory = nullptr;
    IWICBitmapEncoder*      encoder = nullptr;
    IWICBitmapFrameEncode*  frame   = nullptr;
    IPropertyBag2*          props   = nullptr;
    IStream*                stream  = nullptr;

    auto fail = [&](const wchar_t* msg) {
        release(frame);
        release(props);
        release(encoder);
        release(stream);
        release(factory);
        ::DeleteFileW(temp.c_str());
        out.error = msg;
        return out;
    };

    // Created, used and released entirely on this thread: the UI is an STA and
    // there is no marshalling anywhere in this codebase.
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
        return fail(L"The Windows imaging component could not be started.");

    if (FAILED(::SHCreateStreamOnFileEx(temp.c_str(),
                                        STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream)))
        return fail(L"The destination file could not be created. Check the folder and "
                    L"that there is free space.");

    if (FAILED(factory->CreateEncoder(container_for(req.format), nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
        return fail(L"This system has no encoder for that format.");

    if (FAILED(encoder->CreateNewFrame(&frame, &props)))
        return fail(L"The image could not be prepared for writing.");

    if (props) {
        PROPBAG2 opt = {};
        VARIANT  val;
        ::VariantInit(&val);
        if (req.format == Format::Jpeg) {
            opt.pstrName = const_cast<wchar_t*>(L"ImageQuality");
            val.vt = VT_R4;
            val.fltVal = std::clamp(req.jpeg_quality, 1, 100) / 100.0f;
            props->Write(1, &opt, &val);
        } else if (req.format == Format::Tiff) {
            opt.pstrName = const_cast<wchar_t*>(L"TiffCompressionMethod");
            val.vt = VT_UI1;
            // Deflate compresses 16-bit sensor noise materially better than LZW
            // and is read by every tool an astrophotographer is likely to use.
            val.bVal = WICTiffCompressionZIP;
            props->Write(1, &opt, &val);
        } else {
            opt.pstrName = const_cast<wchar_t*>(L"InterlaceOption");
            val.vt = VT_BOOL;
            val.boolVal = VARIANT_FALSE;
            props->Write(1, &opt, &val);
        }
    }

    if (FAILED(frame->Initialize(props)))
        return fail(L"The image could not be prepared for writing.");
    if (FAILED(frame->SetSize(static_cast<UINT>(m.out_w), static_cast<UINT>(m.out_h))))
        return fail(L"The image size was rejected by the encoder.");
    frame->SetResolution(96.0, 96.0);

    // WIC is allowed to negotiate a different pixel format. A silently
    // downgraded 48-bit TIFF would lose half its depth invisibly, which is
    // exactly the failure this feature exists to avoid.
    WICPixelFormatGUID actual = wanted;
    if (FAILED(frame->SetPixelFormat(&actual)) || !::IsEqualGUID(actual, wanted)) {
        if (req.format == Format::Tiff)
            return fail(L"This system's TIFF encoder does not support 16-bit output.");
        return fail(L"This system's encoder does not support the required pixel format.");
    }

    {
        IWICMetadataQueryWriter* meta = nullptr;
        if (SUCCEEDED(frame->GetMetadataQueryWriter(&meta)) && meta) {
            const MetaPaths p = meta_paths_for(req.format);
            put_meta(meta, p.software, L"WinStellar " + widen(req.app_version));
            put_meta(meta, p.description, description);
            release(meta);
        }
    }

    // WritePixels appends sequentially, so blocks of rows stream straight out.
    const UINT stride = static_cast<UINT>(m.out_w) * bytes_per_pixel;
    const int  kBlock = 256;
    std::vector<uint8_t> staging(static_cast<size_t>(stride) * kBlock);
    for (int y = 0; y < m.out_h; y += kBlock) {
        if (is_cancelled && is_cancelled()) {
            release(frame); release(props); release(encoder); release(stream); release(factory);
            ::DeleteFileW(temp.c_str());
            out.cancelled = true;
            return out;
        }
        const int rows = std::min(kBlock, m.out_h - y);
        fill(y, rows, staging.data());
        if (FAILED(frame->WritePixels(static_cast<UINT>(rows), stride,
                                      stride * static_cast<UINT>(rows), staging.data())))
            return fail(L"Writing the image data failed. The disk may be full.");
    }

    if (FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return fail(L"The file could not be finalised. The disk may be full.");

    // Release the stream BEFORE renaming: SHCreateStreamOnFileEx holds an open
    // handle, and MoveFileEx over it fails with a sharing violation.
    release(frame);
    release(props);
    release(encoder);
    release(stream);
    release(factory);

    if (!::MoveFileExW(temp.c_str(), req.dest_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temp.c_str());
        out.error = L"The exported file could not be moved into place. It may be open in "
                    L"another application.";
        return out;
    }

    out.success = true;
    return out;
}

ExportOutcome export_fits(const ExportRequest& req, const std::function<bool()>& is_cancelled) {
    ExportOutcome out;
    out.token = req.token;
    out.dest_path = req.dest_path;

    fitsx::FitsExportOptions opt;
    opt.display_rotation_deg = req.display_rotation_deg;
    opt.source_path = req.source_path;
    opt.expect_width  = req.image ? req.image->width  : 0;
    opt.expect_height = req.image ? req.image->height : 0;
    opt.app_version = req.app_version;
    {
        const size_t slash = req.source_path.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos)
                                ? req.source_path : req.source_path.substr(slash + 1);
        std::string narrow(base.size(), '?');
        for (size_t i = 0; i < base.size(); ++i)
            narrow[i] = (base[i] < 128) ? static_cast<char>(base[i]) : '?';
        opt.source_name = narrow;
    }
    opt.mode = req.allow_memory_fits_fallback ? fitsx::FitsExportMode::Auto
                                              : fitsx::FitsExportMode::FromSourceOnly;
    if (req.source_path.empty())
        opt.mode = fitsx::FitsExportMode::FromImageOnly;

    static const fitsx::FitsImage kEmpty;
    const fitsx::FitsImage& img = req.image ? *req.image : kEmpty;
    const fitsx::FitsExportResult r =
        fitsx::write_fits_export(req.dest_path, img, opt, is_cancelled);

    out.success = r.success;
    out.cancelled = r.cancelled;
    out.fits_carries_cfa = r.carries_cfa;
    if (!r.success && !r.cancelled)
        out.error = widen(r.error.empty() ? "The FITS file could not be written." : r.error);
    return out;
}

}  // namespace

const wchar_t* default_extension(Format f) noexcept {
    switch (f) {
        case Format::Png:  return L"png";
        case Format::Tiff: return L"tif";
        case Format::Fits: return L"fits";
        default:           return L"jpg";
    }
}

ExportOutcome run_export(const ExportRequest& req, const std::function<bool()>& is_cancelled) {
    ExportOutcome out;
    out.token = req.token;
    out.dest_path = req.dest_path;

    if (!req.image || req.image->empty()) {
        out.error = L"No image is loaded.";
        return out;
    }
    if (req.dest_path.empty()) {
        out.error = L"No destination was chosen.";
        return out;
    }

    if (req.format == Format::Fits) return export_fits(req, is_cancelled);

    const fitsx::FitsImage& img = *req.image;
    const fitsx::RotMap m = fitsx::make_rot_map(img.width, img.height, req.display_rotation_deg);

    if (req.format == Format::Tiff) {
        // Linear lane: the source data, not the display.
        const fitsx::Linear16Scale scale = fitsx::compute_linear16_scale(img);
        const std::wstring desc = linear_description(scale, m.deg, req.app_version);
        if (img.is_rgb()) {
            return encode_with_wic(req, m, GUID_WICPixelFormat48bppRGB, 6, desc,
                                   [&](int y0, int rows, void* dst) {
                                       fitsx::pack_rgb48_rows(img, m, scale, y0, rows,
                                                              static_cast<uint16_t*>(dst));
                                   }, is_cancelled);
        }
        return encode_with_wic(req, m, GUID_WICPixelFormat16bppGray, 2, desc,
                               [&](int y0, int rows, void* dst) {
                                   fitsx::pack_gray16_rows(img, m, scale, y0, rows,
                                                           static_cast<uint16_t*>(dst));
                               }, is_cancelled);
    }

    // Display lane: exactly what the window shows. Re-rendered here rather than
    // borrowed from the UI thread, which owns and reassigns its own buffer.
    const fitsx::RenderedBitmap rb = fitsx::render_to_bgra(img, req.stretch);
    if (rb.width != img.width || rb.height != img.height) {
        out.error = L"The image could not be rendered for export.";
        return out;
    }
    const std::wstring desc =
        widen(header_or(img, "OBJECT", "") ) +
        (img.find_header("OBJECT") ? L" — " : L"") +
        L"exported by WinStellar " + widen(req.app_version);

    return encode_with_wic(req, m, GUID_WICPixelFormat24bppBGR, 3, desc,
                           [&](int y0, int rows, void* dst) {
                               fitsx::pack_bgr24_rows(rb, m, y0, rows,
                                                      static_cast<uint8_t*>(dst));
                           }, is_cancelled);
}

}  // namespace wsx
