#include "TiltView.h"

#include "InspectColor.h"

#include <dwmapi.h>
#include <dwrite.h>

#include <algorithm>
#include <cstdio>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarTilt";
constexpr COLORREF kBgColorRef = RGB(0x0b, 0x0d, 0x10);
const D2D1::ColorF kPanelBg(0x05070a);
const D2D1::ColorF kSquare (0x556070);
const D2D1::ColorF kText   (0xE0E2E5);

template <typename T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Displayed 3x3 grid cell (dr,dc) -> source cell index under cw rotation.
int src_cell(int dr, int dc, int rot) {
    int sr, sc;
    switch (rot) {
        case 90:  sr = 2 - dc; sc = dr;     break;
        case 180: sr = 2 - dr; sc = 2 - dc; break;
        case 270: sr = dc;     sc = 2 - dr; break;
        default:  sr = dr;     sc = dc;     break;
    }
    return sr * 3 + sc;
}

// quality_color() (HFR green->amber->red ramp) lives in InspectColor.h, shared
// with the on-image star markers.

}  // namespace

bool TiltWindow::create(HWND owner, HINSTANCE hinst) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon   = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    ::RegisterClassExW(&wc);

    const int w = 300, h = 340;
    RECT or_rc{};
    if (owner) ::GetWindowRect(owner, &or_rc);
    const int x = (owner ? or_rc.right - w - 32 : CW_USEDEFAULT);
    const int y = (owner ? or_rc.top   + 80     : CW_USEDEFAULT);

    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kClassName, L"Tilt",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        x, y, w, h, owner, nullptr, hinst, this);
    if (!hwnd_) return false;

    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    COLORREF capt = kBgColorRef;
    ::DwmSetWindowAttribute(hwnd_, 35, &capt, sizeof(capt));
    ::DwmSetWindowAttribute(hwnd_, 34, &capt, sizeof(capt));
    COLORREF text = RGB(0xE8, 0xEA, 0xED);
    ::DwmSetWindowAttribute(hwnd_, 36, &text, sizeof(text));

    init_d2d();
    return true;
}

void TiltWindow::destroy() {
    release_d2d();
    safe_release(text_);
    safe_release(head_);
    safe_release(dwrite_factory_);
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void TiltWindow::show() { if (hwnd_) ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE); }
void TiltWindow::hide() { if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE); }

void TiltWindow::clear() {
    have_tilt_ = false;
    tilt_ = {};
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void TiltWindow::set_tilt(const fitsx::TiltResult& t) {
    tilt_ = t;
    have_tilt_ = t.success && t.cells.size() == 9;
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void TiltWindow::set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == rot_) return;
    rot_ = deg;
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);   // re-permute on next paint
}

void TiltWindow::init_d2d() {
    if (!d2d_factory_)
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (!dwrite_factory_)
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (dwrite_factory_ && !text_)
        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &text_);
    if (dwrite_factory_ && !head_)
        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &head_);
    if (!d2d_factory_ || !hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    d2d_factory_->CreateHwndRenderTarget(props, hprops, &rt_);
    if (rt_) rt_->CreateSolidColorBrush(kText, &brush_);
}

void TiltWindow::release_d2d() {
    safe_release(brush_);
    safe_release(rt_);
}

void TiltWindow::render() {
    if (!rt_) { init_d2d(); if (!rt_) return; }
    rt_->BeginDraw();
    rt_->Clear(kPanelBg);

    RECT rc; ::GetClientRect(hwnd_, &rc);
    const float W = static_cast<float>(rc.right);
    const float H = static_cast<float>(rc.bottom);

    if (!have_tilt_ || !brush_) {
        if (text_) {
            brush_->SetColor(kText);
            rt_->DrawTextW(L"No stars analysed", 17, text_,
                           D2D1::RectF(10, H * 0.5f - 10, W - 10, H * 0.5f + 12), brush_);
        }
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
        return;
    }

    // Header: tilt + curvature.
    if (head_) {
        wchar_t h[96];
        const int n = swprintf_s(h, L"Tilt %.1f %%      Courbure %.1f %%",
                                 tilt_.tilt_pct, tilt_.curvature_pct);
        brush_->SetColor(kText);
        if (n > 0) rt_->DrawTextW(h, static_cast<UINT32>(n), head_,
                                  D2D1::RectF(14, 10, W - 8, 34), brush_);
    }

    // Square area: a centred square below the header, shrunk to ~62 % of the
    // available room so the deformed corners (up to ~1.5x outward) and their
    // value labels stay inside the window.
    const float top = 46.0f, bottom = 30.0f, inset = 34.0f;
    const float avail_w = W - 2 * inset;
    const float avail_h = H - top - bottom - inset;
    const float side = std::max(40.0f, std::min(avail_w, avail_h) * 0.62f);
    const float sx0 = (W - side) * 0.5f;
    const float sy0 = top + (H - top - bottom - side) * 0.5f;
    const float sx1 = sx0 + side, sy1 = sy0 + side;
    const float mx = (sx0 + sx1) * 0.5f, my = (sy0 + sy1) * 0.5f;

    // Nominal (undistorted) square, faint -- the reference the deformed quad
    // deviates from. A perfectly flat field => deformed quad ≈ this square.
    brush_->SetColor(kSquare); brush_->SetOpacity(0.28f);
    rt_->DrawRectangle(D2D1::RectF(sx0, sy0, sx1, sy1), brush_, 1.0f);

    // Node order TL, TR, BR, BL, centre -> source cells, permuted by rotation
    // so the displayed diagram matches the on-screen orientation.
    const int           ci[5]  = {
        src_cell(0, 0, rot_), src_cell(0, 2, rot_), src_cell(2, 2, rot_),
        src_cell(2, 0, rot_), src_cell(1, 1, rot_) };
    const D2D1_POINT_2F nom[5] = { {sx0,sy0},{sx1,sy0},{sx1,sy1},{sx0,sy1},{mx,my} };
    const float         lox[5] = {  8.0f, -52.0f, -52.0f,  8.0f, -22.0f };
    const float         loy[5] = {  8.0f,   8.0f, -24.0f, -24.0f, 10.0f };

    // Deform each corner along its diagonal by its HFR excess over the corner
    // MEAN, so the quad leans toward the soft corner(s). Relative to the mean
    // (not the min) keeps the distortion centred on the nominal square. The
    // colour ramp stays relative to the sharpest node (red at +20 %).
    double cmean = 0.0; int cn = 0;
    double cmin = 1e30;
    for (int k = 0; k < 5; ++k) {
        const auto& c = tilt_.cells[static_cast<size_t>(ci[k])];
        if (c.count > 0) cmin = std::min(cmin, c.hfr_median);
    }
    for (int k = 0; k < 4; ++k) {
        const auto& c = tilt_.cells[static_cast<size_t>(ci[k])];
        if (c.count > 0) { cmean += c.hfr_median; ++cn; }
    }
    if (cmin >= 1e30) cmin = 1.0;
    const double ref_def = (cn > 0) ? cmean / cn : 1.0;
    constexpr float kGain = 3.0f;

    D2D1_POINT_2F def[5];
    for (int k = 0; k < 4; ++k) {
        const auto& c = tilt_.cells[static_cast<size_t>(ci[k])];
        float factor = 1.0f;
        if (c.count > 0 && ref_def > 0.0) {
            factor = 1.0f + kGain * static_cast<float>((c.hfr_median - ref_def) / ref_def);
            factor = std::clamp(factor, 0.60f, 1.55f);
        }
        def[k] = D2D1::Point2F(mx + (nom[k].x - mx) * factor,
                               my + (nom[k].y - my) * factor);
    }
    def[4] = D2D1::Point2F(mx, my);   // centre stays put

    // Deformed quad outline + diagonals (opposite corners are colinear through
    // the centre, so each diagonal is one straight line).
    brush_->SetColor(kSquare); brush_->SetOpacity(0.95f);
    rt_->DrawLine(def[0], def[1], brush_, 1.4f);
    rt_->DrawLine(def[1], def[2], brush_, 1.4f);
    rt_->DrawLine(def[2], def[3], brush_, 1.4f);
    rt_->DrawLine(def[3], def[0], brush_, 1.4f);
    rt_->DrawLine(def[0], def[2], brush_, 1.4f);
    rt_->DrawLine(def[1], def[3], brush_, 1.4f);

    for (int k = 0; k < 5; ++k) {
        const auto& c = tilt_.cells[static_cast<size_t>(ci[k])];
        const D2D1_POINT_2F p = def[k];
        if (c.count > 0) {
            const float t = static_cast<float>((c.hfr_median - cmin) / (0.20 * cmin));
            brush_->SetColor(quality_color(t)); brush_->SetOpacity(1.0f);
            rt_->FillEllipse(D2D1::Ellipse(p, 6.0f, 6.0f), brush_);
            if (text_) {
                wchar_t v[16];
                const int vn = swprintf_s(v, L"%.2f", c.hfr_median);
                brush_->SetColor(D2D1::ColorF(0xF0F2F5));
                if (vn > 0) rt_->DrawTextW(v, static_cast<UINT32>(vn), text_,
                    D2D1::RectF(p.x + lox[k], p.y + loy[k],
                                p.x + lox[k] + 50, p.y + loy[k] + 18), brush_);
            }
        } else {
            brush_->SetColor(D2D1::ColorF(0x555b63)); brush_->SetOpacity(0.8f);
            rt_->FillEllipse(D2D1::Ellipse(p, 3.5f, 3.5f), brush_);
        }
    }

    // Footer legend.
    if (text_) {
        brush_->SetColor(D2D1::ColorF(0x8b93a0)); brush_->SetOpacity(1.0f);
        rt_->DrawTextW(L"HFR (px) · vert = net  rouge = mou", 31, text_,
                       D2D1::RectF(10, H - 24, W - 10, H - 6), brush_);
    }
    brush_->SetOpacity(1.0f);

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
}

void TiltWindow::on_paint() {
    PAINTSTRUCT ps; ::BeginPaint(hwnd_, &ps); render(); ::EndPaint(hwnd_, &ps);
}

void TiltWindow::on_size(int cx, int cy) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1, cx), std::max(1, cy)));
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK TiltWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TiltWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT:      self->on_paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:       self->on_size(LOWORD(lp), HIWORD(lp)); return 0;
        case WM_CLOSE:      self->hide(); return 0;
        case WM_DESTROY:    return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
