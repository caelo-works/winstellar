#pragma once

#include "fits_core/fits_image.h"

#include <windows.h>
#include <objbase.h>   // WIN32_LEAN_AND_MEAN drops COM from windows.h

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wsx {

// Image export, with no HWND, no ViewerWindow and no globals, so the export
// worker and the headless --export CLI run exactly the same code.
//
// Two data paths, and the difference is the whole point:
//   JPEG / PNG  -- what is on screen, stretch applied, 8 bits per channel.
//   TIFF / FITS -- the LINEAR data, unstretched, at full precision.
// Rotation applies to all four.

enum class Format { Jpeg, Png, Tiff, Fits };

// RAII COM apartment for a worker thread. RPC_E_CHANGED_MODE means the thread
// is already inside an apartment of another kind -- that apartment is usable,
// so ok() accepts it, but we must not unbalance someone else's CoInitializeEx.
struct ComApartment {
    HRESULT hr;
    bool    owns;
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)), owns(SUCCEEDED(hr)) {}
    ~ComApartment() { if (owns) ::CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

struct ExportRequest {
    Format       format = Format::Jpeg;
    std::wstring dest_path;
    std::wstring source_path;                        // the file the image came from
    std::shared_ptr<const fitsx::FitsImage> image;    // linear snapshot, shared with the UI
    fitsx::StretchParams stretch{};                   // JPEG / PNG only
    int  display_rotation_deg = 0;
    int  jpeg_quality = 95;
    // The user was told the original file could not be re-read and accepted a
    // FITS built from the debayered image instead.
    bool allow_memory_fits_fallback = false;
    std::string app_version;
    std::uint64_t token = 0;                          // echoed back, to drop stale results
};

struct ExportOutcome {
    std::uint64_t token = 0;
    bool success = false;
    bool cancelled = false;
    bool fits_carries_cfa = false;
    std::wstring dest_path;
    std::wstring error;    // a finished English sentence, empty on success
};

// Encode and write. Never leaves a partial file behind: everything is written
// to a temp file beside the destination and renamed into place on success.
[[nodiscard]] ExportOutcome run_export(const ExportRequest& req,
                                       const std::function<bool()>& is_cancelled);

// Default file extension for a format ("jpg", "png", "tif", "fits").
[[nodiscard]] const wchar_t* default_extension(Format f) noexcept;

}  // namespace wsx
