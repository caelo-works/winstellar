#include "BackgroundView.h"

#include <dwmapi.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarBackground";
constexpr COLORREF kBgColorRef = RGB(0x0b, 0x0d, 0x10);
const D2D1::ColorF kPanelBg(0x14141a);
const D2D1::ColorF kText   (0xE0E2E5);
const D2D1::ColorF kArrow  (0.0f, 0.90f, 1.0f);

constexpr int kHdr = 30;   // header band

template <typename T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// magma-like LUT (his _colormap), t in [0,1] -> RGB.
void magma(float t, uint8_t& R, uint8_t& G, uint8_t& B) {
    static const float stops[8][3] = {
        {0,0,4},{40,11,84},{101,21,110},{159,42,99},
        {212,72,66},{245,125,21},{250,193,39},{252,255,164}};
    t = std::clamp(t, 0.0f, 1.0f) * 7.0f;
    int i = std::min(6, static_cast<int>(t));
    const float f = t - i;
    R = static_cast<uint8_t>(stops[i][0] * (1 - f) + stops[i + 1][0] * f);
    G = static_cast<uint8_t>(stops[i][1] * (1 - f) + stops[i + 1][1] * f);
    B = static_cast<uint8_t>(stops[i][2] * (1 - f) + stops[i + 1][2] * f);
}

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0.0f;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Rotate an R x C BGRA grid clockwise by rot; outputs rotated dims (dR,dC).
void rotate_grid(const std::vector<uint8_t>& s, int R, int C, int rot,
                 std::vector<uint8_t>& d, int& dR, int& dC) {
    if (rot == 0) { d = s; dR = R; dC = C; return; }
    if (rot == 180) { dR = R; dC = C; } else { dR = C; dC = R; }
    d.resize(static_cast<size_t>(dR) * dC * 4);
    for (int i = 0; i < dR; ++i)
        for (int j = 0; j < dC; ++j) {
            int sr, sc;
            switch (rot) {
                case 90:  sr = R - 1 - j; sc = i;         break;
                case 180: sr = R - 1 - i; sc = C - 1 - j; break;
                case 270: sr = j;         sc = C - 1 - i; break;
                default:  sr = i;         sc = j;         break;
            }
            std::memcpy(&d[(static_cast<size_t>(i) * dC + j) * 4],
                        &s[(static_cast<size_t>(sr) * C + sc) * 4], 4);
        }
}

}  // namespace

bool BackgroundWindow::create(HWND owner, HINSTANCE hinst) {
    hinst_ = hinst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon   = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    ::RegisterClassExW(&wc);

    RECT or_rc{};
    if (owner) ::GetWindowRect(owner, &or_rc);
    const int x = (owner ? or_rc.right - 520 : CW_USEDEFAULT);
    const int y = (owner ? or_rc.top + 110 : CW_USEDEFAULT);

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Carte du fond",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
        x, y, 480, 380, owner, nullptr, hinst, this);
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

void BackgroundWindow::destroy() {
    release_d2d();
    safe_release(text_);
    safe_release(dwrite_factory_);
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void BackgroundWindow::show() { if (hwnd_) ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE); }
void BackgroundWindow::hide() { if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE); }

void BackgroundWindow::clear() {
    have_map_ = false; map_ = {};
    heat_bgra_.clear();
    safe_release(heat_);
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void BackgroundWindow::set_map(const fitsx::BackgroundMap& m) {
    map_ = m;
    have_map_ = m.success && m.rows > 0 && m.cols > 0
             && static_cast<int>(m.surface.size()) == m.rows * m.cols;
    rebuild_heatmap();
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void BackgroundWindow::set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == rot_) return;
    rot_ = deg;
    rebuild_heatmap();   // rotate the existing surface; no recompute needed
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void BackgroundWindow::rebuild_heatmap() {
    safe_release(heat_);
    heat_bgra_.clear();
    heat_w_ = heat_h_ = 0;
    if (!have_map_) return;
    const int R = map_.rows, C = map_.cols;
    std::vector<float> s(map_.surface.begin(), map_.surface.end());
    const float lo = percentile(s, 0.02), hi = percentile(s, 0.98);
    const float span = std::max(hi - lo, 1e-9f);
    std::vector<uint8_t> base(static_cast<size_t>(R) * C * 4);
    for (int i = 0; i < R * C; ++i) {
        uint8_t r8, g8, b8;
        magma((map_.surface[static_cast<size_t>(i)] - lo) / span, r8, g8, b8);
        base[i * 4 + 0] = b8;   // BGRA
        base[i * 4 + 1] = g8;
        base[i * 4 + 2] = r8;
        base[i * 4 + 3] = 255;
    }
    rotate_grid(base, R, C, rot_, heat_bgra_, heat_h_, heat_w_);  // rows->h, cols->w
}

void BackgroundWindow::init_d2d() {
    if (!d2d_factory_)
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (!dwrite_factory_)
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (dwrite_factory_ && !text_)
        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &text_);
    if (!d2d_factory_ || !hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    d2d_factory_->CreateHwndRenderTarget(props, hprops, &rt_);
    if (rt_) rt_->CreateSolidColorBrush(kText, &brush_);
}

void BackgroundWindow::release_d2d() {
    safe_release(heat_);
    safe_release(brush_);
    safe_release(rt_);
}

void BackgroundWindow::render() {
    if (!rt_) { init_d2d(); if (!rt_) return; }
    rt_->BeginDraw();
    rt_->Clear(kPanelBg);

    RECT rc; ::GetClientRect(hwnd_, &rc);
    const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);

    if (text_) {
        brush_->SetColor(kText);
        rt_->DrawTextW(L"Carte du fond / illumination", 28, text_,
                       D2D1::RectF(12, 7, W - 8, kHdr), brush_);
    }

    if (map_.skipped) {
        if (text_) {
            brush_->SetColor(D2D1::ColorF(0x8b93a0));
            rt_->DrawTextW(L"Image trop petite pour la carte de fond.", 39, text_,
                D2D1::RectF(12, H * 0.5f - 10, W - 8, H * 0.5f + 12), brush_);
        }
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
        return;
    }
    if (!have_map_) {
        if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
        return;
    }

    // Heatmap fills the area below the header / above a caption band.
    const float capH = 22.0f;
    const D2D1_RECT_F area = D2D1::RectF(0, kHdr, W, H - capH);
    if (!heat_ && !heat_bgra_.empty() && heat_w_ > 0 && heat_h_ > 0) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        rt_->CreateBitmap(D2D1::SizeU(heat_w_, heat_h_),
                          heat_bgra_.data(), heat_w_ * 4, bp, &heat_);
    }
    if (heat_)
        rt_->DrawBitmap(heat_, area, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

    // Gradient arrow from the heatmap centre.
    const float cx = (area.left + area.right) * 0.5f;
    const float cy = (area.top + area.bottom) * 0.5f;
    const float ang = static_cast<float>(map_.plane_direction_deg + rot_) * 3.14159265f / 180.0f;
    const float reach = std::min(area.right - area.left, area.bottom - area.top) * 0.30f;
    const float ln = reach * std::clamp(static_cast<float>(map_.plane_amplitude_rel) * 5.0f, 0.2f, 1.0f);
    const float ex = cx + ln * std::cos(ang), ey = cy + ln * std::sin(ang);
    brush_->SetColor(kArrow);
    rt_->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(ex, ey), brush_, 3.0f);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ex, ey), 4.0f, 4.0f), brush_);

    // Caption band: the headline metrics.
    if (text_) {
        wchar_t cap[160];
        const int n = swprintf_s(cap,
            L"chute radiale %+.0f%%   gradient %.0f%% @%.0f°   azim %.2f   glow %.0fσ",
            map_.radial_drop * 100.0, map_.plane_amplitude_rel * 100.0,
            map_.plane_direction_deg, map_.azim_aniso, map_.corner_excess_max);
        brush_->SetColor(D2D1::ColorF(0xF0E882));
        if (n > 0)
            rt_->DrawTextW(cap, static_cast<UINT32>(n), text_,
                           D2D1::RectF(8, H - capH + 2, W - 4, H - 2), brush_);
    }

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
}

void BackgroundWindow::on_paint() {
    PAINTSTRUCT ps; ::BeginPaint(hwnd_, &ps); render(); ::EndPaint(hwnd_, &ps);
}

void BackgroundWindow::on_size(int cx, int cy) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1, cx), std::max(1, cy)));
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK BackgroundWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<BackgroundWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
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
