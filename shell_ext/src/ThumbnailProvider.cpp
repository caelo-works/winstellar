#include "ThumbnailProvider.h"
#include "Guids.h"

#include "fits_core/fits_loader.h"
#include "fits_core/fits_render.h"
#include "fits_core/fits_stretch.h"

#include <new>

FitsThumbnailProvider::FitsThumbnailProvider() { DllAddRef(); }
FitsThumbnailProvider::~FitsThumbnailProvider() { DllRelease(); }

HRESULT FitsThumbnailProvider::CreateInstance(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* p = new (std::nothrow) FitsThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

IFACEMETHODIMP FitsThumbnailProvider::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
        *ppv = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IThumbnailProvider) {
        *ppv = static_cast<IThumbnailProvider*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) FitsThumbnailProvider::AddRef() {
    return InterlockedIncrement(&ref_);
}

IFACEMETHODIMP_(ULONG) FitsThumbnailProvider::Release() {
    const ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

IFACEMETHODIMP FitsThumbnailProvider::Initialize(IStream* stream, DWORD /*grfMode*/) {
    buf_.clear();
    return buf_.init(stream);
}

IFACEMETHODIMP FitsThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* phbmp,
                                                    WTS_ALPHATYPE* pdwAlpha) {
    if (!phbmp || !pdwAlpha) return E_POINTER;
    *phbmp = nullptr;
    *pdwAlpha = WTSAT_RGB;

    if (buf_.empty()) return E_FAIL;

    // A hostile or corrupt file can make the loaders throw (bad_alloc /
    // length_error from header-driven vector sizing). This handler runs
    // in-process inside explorer.exe, so an exception escaping across the COM
    // boundary crashes the host. Contain everything and fail the thumbnail.
    try {
        // prefer_fast: thumbnails are downsampled to ~cx px, so a half-resolution
        // RAW decode (LibRaw half_size) is visually identical and ~4x faster.
        auto loaded = fitsx::load_from_memory(buf_.data(), buf_.size(), /*prefer_fast=*/true);
        if (!loaded.success) return E_FAIL;

        const auto stretch = fitsx::compute_auto_stretch(loaded.image);
        const auto bmp = fitsx::render_to_bgra(loaded.image, stretch,
                                                static_cast<int>(cx),
                                                static_cast<int>(cx));
        if (bmp.width <= 0 || bmp.height <= 0) return E_FAIL;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bmp.width;
        bi.bmiHeader.biHeight = -bmp.height;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP h = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!h || !bits) {
            if (h) ::DeleteObject(h);
            return E_FAIL;
        }
        memcpy(bits, bmp.bgra.data(), bmp.bgra.size());
        *phbmp = h;
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
