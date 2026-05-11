#include "Histogram.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarHistogram";

// App theme — kept in sync with ViewerWindow's palette.
constexpr COLORREF kBgColorRef = RGB(0x0b, 0x0d, 0x10);
const D2D1::ColorF kBg     (0x0b0d10);
const D2D1::ColorF kPanelBg(0x05070a);
const D2D1::ColorF kBars   (0x4f5b6a);
const D2D1::ColorF kAccent (0x7dd3fc);
const D2D1::ColorF kText   (0xE0E2E5);
const D2D1::ColorF kGuide  (0x22262c);

constexpr int  kCmd_Auto = 200;
constexpr int  kCmd_Raw  = 201;

constexpr int  kButtonW = 72;
constexpr int  kButtonH = 26;
constexpr int  kButtonGap = 8;

constexpr int  kButtonRowH      = 36;   // bottom strip with Auto/Reset
constexpr int  kSliderStripH    = 28;   // handles + their captions
constexpr int  kReadoutRowH     = 22;   // numeric values row
constexpr int  kInnerPadding    = 12;
constexpr float kHandleHalfW    = 7.0f; // half-width of the diamond handle
constexpr float kHandleHeight   = 18.0f;

}  // namespace

bool HistogramWindow::create(HWND owner, HINSTANCE hinst) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // D2D paints everything
    wc.lpszClassName = kClassName;
    // Reuse the main icon (resource ID 1) so the popup gets a proper taskbar
    // / titlebar icon if Windows ever shows one.
    wc.hIcon   = static_cast<HICON>(::LoadImageW(
        hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(
        hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    ::RegisterClassExW(&wc);

    // Position: docked to the right of the owner, same top, ~520x340.
    RECT or_rc{};
    if (owner) ::GetWindowRect(owner, &or_rc);
    const int w = 520, h = 340;
    const int x = (owner ? or_rc.right - w - 32 : CW_USEDEFAULT);
    const int y = (owner ? or_rc.top   + 64     : CW_USEDEFAULT);

    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kClassName, L"Histogram",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, owner, nullptr, hinst, this);
    if (!hwnd_) return false;

    // Dark caption to match the rest of the app.
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, /*USE_IMMERSIVE_DARK_MODE*/ 20, &dark, sizeof(dark));
    COLORREF capt = kBgColorRef;
    ::DwmSetWindowAttribute(hwnd_, /*CAPTION_COLOR*/ 35, &capt, sizeof(capt));
    ::DwmSetWindowAttribute(hwnd_, /*BORDER_COLOR */ 34, &capt, sizeof(capt));
    COLORREF text = RGB(0xE8, 0xEA, 0xED);
    ::DwmSetWindowAttribute(hwnd_, /*TEXT_COLOR   */ 36, &text, sizeof(text));

    // Owner-drawn child buttons — we paint them in WM_DRAWITEM to match the
    // popup's dark theme rather than rely on system button visual styles.
    btn_auto_ = ::CreateWindowExW(0, L"BUTTON", L"Auto",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCmd_Auto)),
        hinst, nullptr);
    btn_raw_ = ::CreateWindowExW(0, L"BUTTON", L"RAW",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCmd_Raw)),
        hinst, nullptr);

    LOGFONTW lf{};
    lf.lfHeight     = -13;
    lf.lfWeight     = FW_SEMIBOLD;
    lf.lfCharSet    = DEFAULT_CHARSET;
    lf.lfQuality    = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    btn_font_ = ::CreateFontIndirectW(&lf);
    if (btn_font_) {
        ::SendMessageW(btn_auto_, WM_SETFONT, reinterpret_cast<WPARAM>(btn_font_), FALSE);
        ::SendMessageW(btn_raw_,  WM_SETFONT, reinterpret_cast<WPARAM>(btn_font_), FALSE);
    }

    init_d2d();
    return true;
}

void HistogramWindow::destroy() {
    release_d2d();
    if (btn_font_) { ::DeleteObject(btn_font_); btn_font_ = nullptr; }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    btn_auto_ = btn_raw_ = nullptr;
}

void HistogramWindow::show() {
    if (!hwnd_) return;
    // First show after a load (or after a re-show following an image swap):
    // compute the bins now that the user actually wants to see them.
    if (bins_pending_) {
        recompute_bins(*bins_pending_);
        bins_pending_.reset();
    }
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void HistogramWindow::hide() {
    if (!hwnd_) return;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void HistogramWindow::set_image(std::shared_ptr<const fitsx::FitsImage> image) {
    if (!image || image->data.empty()) {
        bins_.clear();
        bins_log_max_ = 0.0f;
        bins_pending_.reset();
    } else {
        bins_pending_ = std::move(image);
        bins_.clear();              // forget the previous image's bars
        bins_log_max_ = 0.0f;
        if (is_visible()) {         // only pay the cost if the popup is open
            recompute_bins(*bins_pending_);
            bins_pending_.reset();
        }
    }
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HistogramWindow::set_params(const fitsx::StretchParams& p) {
    params_ = p;
    apply_params_to_handles();
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HistogramWindow::init_d2d() {
    if (!d2d_factory_) {
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    }
    if (!d2d_factory_ || !hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    d2d_factory_->CreateHwndRenderTarget(props, hprops, &rt_);
    if (!rt_) return;

    // Cache brushes once per render target. Previously the render() loop
    // created/released each brush on every paint -- at 60 Hz drag that was
    // ~360 alloc/release pairs per second.
    rt_->CreateSolidColorBrush(kPanelBg,                                  &br_panel_);
    rt_->CreateSolidColorBrush(kBars,                                     &br_bar_);
    rt_->CreateSolidColorBrush(kGuide,                                    &br_guide_);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f),     &br_clip_);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0x141820),                    &br_strip_);
    rt_->CreateSolidColorBrush(kAccent,                                   &br_handle_);

    // Build a unit diamond geometry centered at origin once. Each render()
    // translates the render-target transform to position it at each handle
    // -- avoids three CreatePathGeometry/Open/Close/Release dances per paint.
    if (!diamond_) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&diamond_)) &&
            SUCCEEDED(diamond_->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(0.0f, -kHandleHeight * 0.5f),
                              D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(kHandleHalfW, 0.0f));
            sink->AddLine(D2D1::Point2F(0.0f, kHandleHeight * 0.5f));
            sink->AddLine(D2D1::Point2F(-kHandleHalfW, 0.0f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
        }
    }
}

void HistogramWindow::release_d2d() {
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(br_panel_); rel(br_bar_); rel(br_guide_);
    rel(br_clip_);  rel(br_strip_); rel(br_handle_);
    rel(diamond_);
    rel(rt_);
    rel(d2d_factory_);
}

LRESULT CALLBACK HistogramWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<HistogramWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        auto* w = static_cast<HistogramWindow*>(cs->lpCreateParams);
        if (w) w->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_PAINT:        self->on_paint(); return 0;
        case WM_ERASEBKGND:   return 1;
        case WM_SIZE:         self->on_size(LOWORD(lp), HIWORD(lp)); return 0;
        case WM_LBUTTONDOWN:  self->on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEMOVE:    self->on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP:    self->on_mouse_up(); return 0;
        case WM_COMMAND:      self->on_command(LOWORD(wp)); return 0;
        case WM_DRAWITEM:
            self->on_drawitem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_CTLCOLORBTN: {
            // Stop the system from painting the button's bg in 3D-face grey
            // before we get our WM_DRAWITEM. Return a brush matching the popup.
            static HBRUSH s_bg = ::CreateSolidBrush(kBgColorRef);
            return reinterpret_cast<LRESULT>(s_bg);
        }
        case WM_CLOSE:        self->hide(); return 0;   // X just hides
        case WM_DESTROY:      return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void HistogramWindow::on_size(int cx, int cy) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1, cx), std::max(1, cy)));

    // Buttons sit bottom-right of the popup: [Auto] [RAW]
    if (btn_auto_ && btn_raw_) {
        const int y    = cy - kInnerPadding - kButtonH;
        const int rawx = cx - kInnerPadding - kButtonW;
        const int autx = rawx - kButtonGap   - kButtonW;
        ::MoveWindow(btn_auto_, autx, y, kButtonW, kButtonH, TRUE);
        ::MoveWindow(btn_raw_,  rawx, y, kButtonW, kButtonH, TRUE);
    }

    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

D2D1_RECT_F HistogramWindow::histogram_rect() const {
    if (!hwnd_) return {};
    RECT rc; ::GetClientRect(hwnd_, &rc);
    const float pad = static_cast<float>(kInnerPadding);
    return D2D1::RectF(
        pad, pad,
        static_cast<float>(rc.right) - pad,
        static_cast<float>(rc.bottom) - pad - kButtonRowH - kReadoutRowH - kSliderStripH);
}

D2D1_RECT_F HistogramWindow::slider_rect() const {
    if (!hwnd_) return {};
    RECT rc; ::GetClientRect(hwnd_, &rc);
    const float pad = static_cast<float>(kInnerPadding);
    const float top = static_cast<float>(rc.bottom) - pad - kButtonRowH - kReadoutRowH - kSliderStripH;
    return D2D1::RectF(
        pad, top,
        static_cast<float>(rc.right) - pad,
        top + kSliderStripH);
}

float HistogramWindow::norm_to_x(float n) const {
    const D2D1_RECT_F hr = histogram_rect();
    return hr.left + std::clamp(n, 0.0f, 1.0f) * (hr.right - hr.left);
}

float HistogramWindow::x_to_norm(float x) const {
    const D2D1_RECT_F hr = histogram_rect();
    if (hr.right <= hr.left) return 0.0f;
    return std::clamp((x - hr.left) / (hr.right - hr.left), 0.0f, 1.0f);
}

int HistogramWindow::hit_test_handle(float x, float y) const {
    const D2D1_RECT_F sr = slider_rect();
    if (y < sr.top || y > sr.bottom) return -1;
    // Prefer midtone if multiple handles overlap (it's the one most users want
    // to grab and it sits between the other two).
    const float vals[3] = { shadows_, midtone_, highlights_ };
    const int   order[3] = { 1, 0, 2 };   // try mid first
    for (int k : order) {
        const float hx = norm_to_x(vals[k]);
        if (std::abs(x - hx) <= kHandleHalfW + 2.0f) return k;
    }
    return -1;
}

void HistogramWindow::on_mouse_down(int x, int y) {
    const int h = hit_test_handle(static_cast<float>(x), static_cast<float>(y));
    if (h < 0) return;
    drag_handle_ = h;
    ::SetCapture(hwnd_);
    on_mouse_move(x, y);   // immediate snap to mouse-x for click-without-drag
}

void HistogramWindow::on_mouse_move(int x, int y) {
    (void)y;
    if (drag_handle_ < 0) return;
    const float n = x_to_norm(static_cast<float>(x));
    constexpr float kMinGap = 0.001f;   // never let two handles coincide exactly
    switch (drag_handle_) {
        case 0:   // shadows: clamp [0, midtone - gap]
            shadows_ = std::min(n, midtone_ - kMinGap);
            shadows_ = std::max(shadows_, 0.0f);
            break;
        case 1:   // midtone: clamp [shadows + gap, highlights - gap]
            midtone_ = std::clamp(n, shadows_ + kMinGap, highlights_ - kMinGap);
            break;
        case 2:   // highlights: clamp [midtone + gap, 1]
            highlights_ = std::max(n, midtone_ + kMinGap);
            highlights_ = std::min(highlights_, 1.0f);
            break;
    }
    apply_handles_to_params();
    if (on_changed_) on_changed_(params_);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HistogramWindow::on_mouse_up() {
    if (drag_handle_ >= 0) {
        drag_handle_ = -1;
        ::ReleaseCapture();
    }
}

void HistogramWindow::on_command(int id) {
    switch (id) {
        case kCmd_Auto: if (on_auto_) on_auto_(); break;
        case kCmd_Raw:  if (on_raw_)  on_raw_();  break;
    }
}

void HistogramWindow::on_drawitem(DRAWITEMSTRUCT* di) {
    if (!di || di->CtlType != ODT_BUTTON) return;
    const bool pressed = (di->itemState & ODS_SELECTED) != 0;
    const bool focused = (di->itemState & ODS_FOCUS)    != 0;

    // Background (matches the toolbar's pressed/hover scheme).
    const COLORREF bg = pressed ? RGB(0x22, 0x28, 0x30)
                                : RGB(0x18, 0x1c, 0x22);
    HBRUSH br = ::CreateSolidBrush(bg);
    ::FillRect(di->hDC, &di->rcItem, br);
    ::DeleteObject(br);

    // Subtle 1px border, slightly brighter when focused.
    HPEN pen = ::CreatePen(PS_SOLID, 1,
        focused ? RGB(0x7d, 0xd3, 0xfc) : RGB(0x2a, 0x32, 0x3c));
    HGDIOBJ oldp = ::SelectObject(di->hDC, pen);
    HGDIOBJ oldb = ::SelectObject(di->hDC, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(di->hDC, di->rcItem.left, di->rcItem.top,
                         di->rcItem.right, di->rcItem.bottom);
    ::SelectObject(di->hDC, oldp);
    ::SelectObject(di->hDC, oldb);
    ::DeleteObject(pen);

    // Label.
    wchar_t label[16] = {};
    ::GetWindowTextW(di->hwndItem, label, _countof(label));
    HGDIOBJ oldf = btn_font_ ? ::SelectObject(di->hDC, btn_font_) : nullptr;
    ::SetTextColor(di->hDC, RGB(0xE0, 0xE2, 0xE5));
    ::SetBkMode(di->hDC, TRANSPARENT);
    ::DrawTextW(di->hDC, label, -1, &di->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldf) ::SelectObject(di->hDC, oldf);
}

void HistogramWindow::apply_handles_to_params() {
    params_.shadows    = shadows_;
    params_.highlights = highlights_;
    // PixInsight-style: midtone handle position maps to MTF m via its position
    // INSIDE the [shadows, highlights] range. 0.5 in that range = identity.
    const float span = std::max(1e-6f, highlights_ - shadows_);
    params_.midtone = std::clamp((midtone_ - shadows_) / span, 0.001f, 0.999f);
}

void HistogramWindow::apply_params_to_handles() {
    shadows_    = std::clamp(params_.shadows,    0.0f, 1.0f);
    highlights_ = std::clamp(params_.highlights, 0.0f, 1.0f);
    if (highlights_ <= shadows_) highlights_ = std::min(1.0f, shadows_ + 0.001f);
    const float span = highlights_ - shadows_;
    midtone_ = shadows_ + std::clamp(params_.midtone, 0.0f, 1.0f) * span;
}

void HistogramWindow::recompute_bins(const fitsx::FitsImage& img) {
    // Integer counter is ~3x faster than float bin in this hot loop (the
    // float add chain isn't autovectorizable; the int increment is).
    // Loaders sanitize NaN/Inf at load time, so no per-pixel isfinite is
    // needed here.
    std::vector<uint32_t> counts(256, 0);
    const float vmin = static_cast<float>(img.source_min);
    const float vmax = static_cast<float>(img.source_max);
    const float scale = 256.0f / std::max(1e-6f, vmax - vmin);
    for (float v : img.data) {
        int bin = static_cast<int>((v - vmin) * scale);
        if (bin < 0) bin = 0;
        if (bin > 255) bin = 255;
        ++counts[bin];
    }
    // Log compression: astro histograms span 5+ orders of magnitude.
    bins_.assign(256, 0.0f);
    bins_log_max_ = 0.0f;
    for (int i = 0; i < 256; ++i) {
        const float b = std::log1p(static_cast<float>(counts[i]));
        bins_[i] = b;
        if (b > bins_log_max_) bins_log_max_ = b;
    }
}

void HistogramWindow::on_paint() {
    PAINTSTRUCT ps;
    ::BeginPaint(hwnd_, &ps);
    if (!rt_) init_d2d();
    if (rt_) render();
    ::EndPaint(hwnd_, &ps);
}

void HistogramWindow::render() {
    if (!rt_) return;
    rt_->BeginDraw();
    rt_->Clear(kBg);

    const D2D1_RECT_F hr = histogram_rect();

    if (br_panel_) rt_->FillRectangle(hr, br_panel_);

    // Bars (log-y, 256 bins -> 256 columns scaled to width).
    if (!bins_.empty() && bins_log_max_ > 0.0f && br_bar_) {
        const float w = hr.right - hr.left;
        const float h = hr.bottom - hr.top;
        const float bar_w = w / 256.0f;
        for (int i = 0; i < 256; ++i) {
            const float bh = (bins_[i] / bins_log_max_) * h;
            D2D1_RECT_F r = D2D1::RectF(
                hr.left + i * bar_w,
                hr.bottom - bh,
                hr.left + (i + 1) * bar_w,
                hr.bottom);
            rt_->FillRectangle(r, br_bar_);
        }
    }

    const float xs = norm_to_x(shadows_);
    const float xh = norm_to_x(highlights_);

    // Clip-region guides (vertical lines at shadows / highlights).
    if (br_guide_) {
        rt_->DrawLine(D2D1::Point2F(xs, hr.top), D2D1::Point2F(xs, hr.bottom), br_guide_, 1.0f);
        rt_->DrawLine(D2D1::Point2F(xh, hr.top), D2D1::Point2F(xh, hr.bottom), br_guide_, 1.0f);
    }

    // Shaded "clipped" zones outside [shadows, highlights].
    if (br_clip_) {
        rt_->FillRectangle(D2D1::RectF(hr.left, hr.top, xs, hr.bottom), br_clip_);
        rt_->FillRectangle(D2D1::RectF(xh, hr.top, hr.right, hr.bottom), br_clip_);
    }

    // Slider strip + 3 diamond handles. The diamond geometry is built once
    // around the origin; we translate the render-target transform to each
    // handle's centerpoint and reuse the same FillGeometry call -- avoids
    // three PathGeometry allocations per paint.
    const D2D1_RECT_F sr = slider_rect();
    if (br_strip_) rt_->FillRectangle(sr, br_strip_);

    if (br_handle_ && diamond_) {
        const float vals[3] = { shadows_, midtone_, highlights_ };
        const float cy_handle = (sr.top + sr.bottom) * 0.5f;
        for (int i = 0; i < 3; ++i) {
            const float cx = norm_to_x(vals[i]);
            rt_->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy_handle));
            // Midtone slightly darker so it reads as a different role.
            br_handle_->SetOpacity(i == 1 ? 0.70f : 1.0f);
            rt_->FillGeometry(diamond_, br_handle_);
        }
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Numeric readouts + button row are text-only for now. We draw text via
    // DirectWrite -- but to keep this skeleton small, we just paint nothing and
    // wire those rows up in a follow-up. For the MVP the diamond positions are
    // enough feedback.

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        release_d2d();
    }
}
