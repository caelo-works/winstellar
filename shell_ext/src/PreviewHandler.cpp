#include "PreviewHandler.h"
#include "Guids.h"

#include "fits_core/fits_loader.h"
#include "fits_core/fits_render.h"
#include "fits_core/fits_stretch.h"

#include <new>
#include <string>

const wchar_t* FitsPreviewHandler::kWndClass = L"FitsPreviewHandlerWindow";

FitsPreviewHandler::FitsPreviewHandler() { DllAddRef(); }

FitsPreviewHandler::~FitsPreviewHandler() {
    release_site();
    destroy_child();
    if (bitmap_) { ::DeleteObject(bitmap_); bitmap_ = nullptr; }
    DllRelease();
}

HRESULT FitsPreviewHandler::CreateInstance(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* p = new (std::nothrow) FitsPreviewHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

IFACEMETHODIMP FitsPreviewHandler::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
        *ppv = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IPreviewHandler) {
        *ppv = static_cast<IPreviewHandler*>(this);
    } else if (riid == IID_IPreviewHandlerVisuals) {
        *ppv = static_cast<IPreviewHandlerVisuals*>(this);
    } else if (riid == IID_IObjectWithSite) {
        *ppv = static_cast<IObjectWithSite*>(this);
    } else if (riid == IID_IOleWindow) {
        *ppv = static_cast<IOleWindow*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) FitsPreviewHandler::AddRef() { return InterlockedIncrement(&ref_); }

IFACEMETHODIMP_(ULONG) FitsPreviewHandler::Release() {
    const ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

void FitsPreviewHandler::release_site() {
    if (site_) { site_->Release(); site_ = nullptr; }
}

void FitsPreviewHandler::destroy_child() {
    if (child_) { ::DestroyWindow(child_); child_ = nullptr; }
}

LRESULT CALLBACK FitsPreviewHandler::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<FitsPreviewHandler*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            if (!self) break;
            PAINTSTRUCT ps = {};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc;
            ::GetClientRect(hwnd, &rc);

            HBRUSH bg = ::CreateSolidBrush(self->bg_);
            ::FillRect(dc, &rc, bg);
            ::DeleteObject(bg);

            if (self->bitmap_ && self->bmp_w_ > 0 && self->bmp_h_ > 0) {
                const int cw = rc.right - rc.left;
                const int ch = rc.bottom - rc.top;
                const double sx = double(cw) / self->bmp_w_;
                const double sy = double(ch) / self->bmp_h_;
                const double s = (sx < sy) ? sx : sy;
                const int dw = (s > 0.0) ? int(self->bmp_w_ * s) : self->bmp_w_;
                const int dh = (s > 0.0) ? int(self->bmp_h_ * s) : self->bmp_h_;
                const int dx = rc.left + (cw - dw) / 2;
                const int dy = rc.top + (ch - dh) / 2;

                HDC mem = ::CreateCompatibleDC(dc);
                HGDIOBJ old = ::SelectObject(mem, self->bitmap_);
                ::SetStretchBltMode(dc, HALFTONE);
                ::SetBrushOrgEx(dc, 0, 0, nullptr);
                ::StretchBlt(dc, dx, dy, dw, dh,
                             mem, 0, 0, self->bmp_w_, self->bmp_h_, SRCCOPY);
                ::SelectObject(mem, old);
                ::DeleteDC(mem);
            } else if (!self->error_text_.empty()) {
                ::SetTextColor(dc, self->fg_);
                ::SetBkMode(dc, TRANSPARENT);
                RECT tr = rc;
                ::DrawTextW(dc, self->error_text_.c_str(), -1, &tr,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void FitsPreviewHandler::create_child_if_needed() {
    if (child_ || !parent_) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &WndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWndClass;
        ::RegisterClassExW(&wc);
        registered = true;
    }

    child_ = ::CreateWindowExW(
        0, kWndClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        rect_.left, rect_.top,
        rect_.right - rect_.left, rect_.bottom - rect_.top,
        parent_, nullptr, g_hInst, this);
}

void FitsPreviewHandler::invalidate() {
    if (child_) ::InvalidateRect(child_, nullptr, FALSE);
}

void FitsPreviewHandler::load_and_render() {
    if (bitmap_) { ::DeleteObject(bitmap_); bitmap_ = nullptr; }
    bmp_w_ = bmp_h_ = 0;
    render_failed_ = false;
    error_text_.clear();

    if (buf_.empty()) {
        render_failed_ = true;
        error_text_ = L"No data";
        return;
    }

    // Runs in-process in explorer.exe; a malformed file can throw from the
    // loaders (bad_alloc / length_error on header-driven sizing). Contain it and
    // show the error state instead of letting the exception crash the host.
    try {
        auto loaded = fitsx::load_from_memory(buf_.data(), buf_.size());
        if (!loaded.success) {
            render_failed_ = true;
            error_text_ = L"Failed to parse FITS";
            return;
        }

        const auto stretch = fitsx::compute_auto_stretch(loaded.image);
        // Preview pane is typically <= 600px wide; render at native then let StretchBlt downscale.
        constexpr int kPreviewMaxDim = 1024;
        auto bmp = fitsx::render_to_bgra(loaded.image, stretch, kPreviewMaxDim, kPreviewMaxDim);
        if (bmp.width <= 0 || bmp.height <= 0) {
            render_failed_ = true;
            error_text_ = L"Empty render";
            return;
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bmp.width;
        bi.bmiHeader.biHeight = -bmp.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP h = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!h || !bits) {
            if (h) ::DeleteObject(h);
            render_failed_ = true;
            error_text_ = L"DIB allocation failed";
            return;
        }
        memcpy(bits, bmp.bgra.data(), bmp.bgra.size());
        bitmap_ = h;
        bmp_w_ = bmp.width;
        bmp_h_ = bmp.height;
    } catch (...) {
        if (bitmap_) { ::DeleteObject(bitmap_); bitmap_ = nullptr; }
        bmp_w_ = bmp_h_ = 0;
        render_failed_ = true;
        error_text_ = L"Malformed or unreadable file";
    }
}

IFACEMETHODIMP FitsPreviewHandler::Initialize(IStream* stream, DWORD /*grfMode*/) {
    // The preview pane host reuses the handler across selections: each new file
    // triggers a fresh Initialize call (after Unload). Reset state instead of
    // refusing — refusing was breaking preview on every other selection.
    buf_.clear();
    if (bitmap_) { ::DeleteObject(bitmap_); bitmap_ = nullptr; }
    bmp_w_ = bmp_h_ = 0;
    render_failed_ = false;
    error_text_.clear();
    return buf_.init(stream);
}

IFACEMETHODIMP FitsPreviewHandler::SetWindow(HWND hwnd, const RECT* prc) {
    parent_ = hwnd;
    if (prc) rect_ = *prc;
    if (child_ && parent_) {
        ::SetParent(child_, parent_);
        ::MoveWindow(child_, rect_.left, rect_.top,
                     rect_.right - rect_.left, rect_.bottom - rect_.top, TRUE);
    }
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::SetRect(const RECT* prc) {
    if (!prc) return E_POINTER;
    rect_ = *prc;
    if (child_) {
        ::MoveWindow(child_, rect_.left, rect_.top,
                     rect_.right - rect_.left, rect_.bottom - rect_.top, TRUE);
    }
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::DoPreview() {
    create_child_if_needed();
    load_and_render();
    invalidate();
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::Unload() {
    destroy_child();
    if (bitmap_) { ::DeleteObject(bitmap_); bitmap_ = nullptr; }
    bmp_w_ = bmp_h_ = 0;
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::SetFocus() {
    if (child_) ::SetFocus(child_);
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::QueryFocus(HWND* phwnd) {
    if (!phwnd) return E_POINTER;
    *phwnd = ::GetFocus();
    return *phwnd ? S_OK : S_FALSE;
}

IFACEMETHODIMP FitsPreviewHandler::TranslateAccelerator(MSG* /*pmsg*/) {
    return S_FALSE;
}

IFACEMETHODIMP FitsPreviewHandler::SetBackgroundColor(COLORREF /*color*/) {
    // Ignore the theme-supplied colour: astrophotos are inherently dark, so a
    // black letterbox always reads better than a white one in light theme.
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::SetFont(const LOGFONTW* /*plf*/) { return S_OK; }

IFACEMETHODIMP FitsPreviewHandler::SetTextColor(COLORREF color) {
    fg_ = color;
    invalidate();
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::SetSite(IUnknown* site) {
    release_site();
    site_ = site;
    if (site_) site_->AddRef();
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::GetSite(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!site_) return E_FAIL;
    return site_->QueryInterface(riid, ppv);
}

IFACEMETHODIMP FitsPreviewHandler::GetWindow(HWND* phwnd) {
    if (!phwnd) return E_POINTER;
    *phwnd = child_ ? child_ : parent_;
    return S_OK;
}

IFACEMETHODIMP FitsPreviewHandler::ContextSensitiveHelp(BOOL /*fEnter*/) {
    return E_NOTIMPL;
}
