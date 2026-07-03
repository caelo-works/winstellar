#include "AberrationView.h"

#include "fits_core/pixmath.h"

#include "InspectRotation.h"

#include <dwmapi.h>
#include <dwrite.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarAberration";

constexpr COLORREF kBgColorRef = RGB(0x0b, 0x0d, 0x10);
const D2D1::ColorF  kPanelBg(0x05070a);
const D2D1::ColorF  kFrame  (0x22262c);
const D2D1::ColorF  kAccent (0x7dd3fc);
const D2D1::ColorF  kEllipse(0.0f, 0.86f, 1.0f);   // cyan, like the proto
const D2D1::ColorF  kCross  (1.0f, 0.31f, 0.31f);  // red centroid

// Toolbar theme — mirrors the main app's Toolbar palette.
constexpr COLORREF kTbBg        = RGB(0x0b, 0x0d, 0x10);
constexpr COLORREF kTbBgHover   = RGB(0x18, 0x1c, 0x22);
constexpr COLORREF kTbBgPressed = RGB(0x22, 0x28, 0x30);
constexpr COLORREF kTbFg        = RGB(0xE0, 0xE2, 0xE5);
constexpr COLORREF kTbAccent    = RGB(0x7d, 0xd3, 0xfc);

constexpr int kGap = 6;
constexpr int kSubGap = 4;       // gap between the 2x2 sub-stamps
constexpr int kIdGrid3   = 101;
constexpr int kIdGrid5   = 102;
constexpr int kIdVisual  = 103;
constexpr int kIdInspect = 104;

constexpr int kBaseTile = 178;   // initial on-screen tile size (3x3)

template <typename T>
void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// luma_at(img, x, y) comes from fits_core/pixmath.h (found via ADL on FitsImage).

// asinh-normalised grayscale stamp -> BGRA (his asinh_norm). cut x cut.
void asinh_stamp(const fitsx::FitsImage& img, int ox, int oy, int cut,
                 std::vector<uint8_t>& out) {
    const size_t n = static_cast<size_t>(cut) * cut;
    std::vector<float> a(n);
    for (int y = 0; y < cut; ++y)
        for (int x = 0; x < cut; ++x)
            a[static_cast<size_t>(y) * cut + x] = luma_at(img, ox + x, oy + y);
    std::vector<float> tmp = a;
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    const float med = tmp[n / 2];
    for (auto& v : a) v = std::max(0.0f, v - med);
    tmp = a;
    const size_t pidx = static_cast<size_t>(0.995 * (n - 1));
    std::nth_element(tmp.begin(), tmp.begin() + pidx, tmp.end());
    const float p = std::max(tmp[pidx], 1e-6f);
    const float denom = std::asinh(1.0f / 0.15f);
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        float v = std::asinh(a[i] / (p * 0.15f)) / denom;
        v = std::clamp(v, 0.0f, 1.0f);
        const uint8_t g = static_cast<uint8_t>(v * 255.0f + 0.5f);
        out[i * 4 + 0] = g; out[i * 4 + 1] = g; out[i * 4 + 2] = g; out[i * 4 + 3] = 255;
    }
}

void blit_crop(const fitsx::RenderedBitmap& rb, int x0, int y0, int C,
               std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(C) * C * 4);
    const int dst_stride = C * 4;
    for (int y = 0; y < C; ++y) {
        const uint8_t* s = rb.bgra.data()
            + static_cast<size_t>(y0 + y) * rb.stride_bytes + static_cast<size_t>(x0) * 4;
        std::memcpy(out.data() + static_cast<size_t>(y) * dst_stride, s,
                    static_cast<size_t>(dst_stride));
    }
}

// Displayed grid cell (dr,dc) -> source grid cell (sr,sc), N x N, cw rotation.

// Rotate an n x n BGRA buffer clockwise by rot (0/90/180/270).

// Rotate a point (x,y) within an n x n box clockwise by rot.

LRESULT CALLBACK bar_subproc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(w);
        RECT rc; ::GetClientRect(h, &rc);
        HBRUSH br = ::CreateSolidBrush(kTbBg);
        ::FillRect(hdc, &rc, br);
        ::DeleteObject(br);
        return 1;
    }
    if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, bar_subproc, id);
    return ::DefSubclassProc(h, m, w, l);
}

const wchar_t* axis_str(fitsx::PsfAxis a) {
    switch (a) {
        case fitsx::PsfAxis::Ref:        return L"ref";
        case fitsx::PsfAxis::Radial:     return L"radial";
        case fitsx::PsfAxis::Tangential: return L"tang.";
        case fitsx::PsfAxis::Oblique:    return L"oblique";
        default:                         return L"";
    }
}

}  // namespace

// Live tile size derived from the (resizable) client area, kept square.
int AberrationWindow::tile_px() const {
    if (!hwnd_) return kBaseTile;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    const int availW = rc.right - (grid_ + 1) * kGap;
    const int availH = (rc.bottom - kBarH) - (grid_ + 1) * kGap;
    const int t = std::min(availW / grid_, availH / grid_);
    return std::max(24, t);
}

bool AberrationWindow::create(HWND owner, HINSTANCE hinst) {
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

    RECT or_rc{};
    if (owner) ::GetWindowRect(owner, &or_rc);
    const int x = (owner ? or_rc.right - 580 : CW_USEDEFAULT);
    const int y = (owner ? or_rc.top   + 96  : CW_USEDEFAULT);

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"Aberration Inspector",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX,
        x, y, 400, 400, owner, nullptr, hinst, this);
    if (!hwnd_) return false;

    create_toolbar();

    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    COLORREF capt = kBgColorRef;
    ::DwmSetWindowAttribute(hwnd_, 35, &capt, sizeof(capt));
    ::DwmSetWindowAttribute(hwnd_, 34, &capt, sizeof(capt));
    COLORREF text = RGB(0xE8, 0xEA, 0xED);
    ::DwmSetWindowAttribute(hwnd_, 36, &text, sizeof(text));

    resize_to_grid();
    init_d2d();
    return true;
}

void AberrationWindow::create_toolbar() {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    ::InitCommonControlsEx(&icc);

    bar_ = ::CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
        0, 0, 0, 0, hwnd_, nullptr, hinst_, nullptr);
    if (!bar_) return;

    ::SendMessageW(bar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    ::SendMessageW(bar_, TB_SETBUTTONSIZE, 0, MAKELPARAM(74, kBarH));
    ::SendMessageW(bar_, TB_SETPADDING, 0, MAKELPARAM(0, 0));
    ::SendMessageW(bar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);

    LOGFONTW lf{};
    lf.lfHeight = -13; lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    bar_font_ = ::CreateFontIndirectW(&lf);

    auto mk = [](int id, int style) {
        TBBUTTON b{}; b.iBitmap = I_IMAGENONE; b.idCommand = id;
        b.fsStyle = static_cast<BYTE>(style); b.fsState = TBSTATE_ENABLED; return b;
    };
    TBBUTTON tb[5] = {
        mk(kIdGrid3,   BTNS_BUTTON), mk(kIdGrid5, BTNS_BUTTON), mk(0, BTNS_SEP),
        mk(kIdVisual,  BTNS_BUTTON), mk(kIdInspect, BTNS_BUTTON),
    };
    ::SendMessageW(bar_, TB_ADDBUTTONS, 5, reinterpret_cast<LPARAM>(tb));
    ::SendMessageW(bar_, TB_CHECKBUTTON, kIdGrid3,  MAKELPARAM(TRUE, 0));
    ::SendMessageW(bar_, TB_CHECKBUTTON, kIdVisual, MAKELPARAM(TRUE, 0));

    ::SetWindowSubclass(bar_, bar_subproc, 1, 0);
}

LRESULT AberrationWindow::on_bar_customdraw(LPNMTBCUSTOMDRAW nm) const {
    switch (nm->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: {
            HBRUSH br = ::CreateSolidBrush(kTbBg);
            ::FillRect(nm->nmcd.hdc, &nm->nmcd.rc, br);
            ::DeleteObject(br);
            return CDRF_NOTIFYITEMDRAW;
        }
        case CDDS_ITEMPREPAINT: {
            HDC hdc = nm->nmcd.hdc;
            RECT rc = nm->nmcd.rc;
            const DWORD st = nm->nmcd.uItemState;
            const int   id = static_cast<int>(nm->nmcd.dwItemSpec);
            COLORREF bg = kTbBg;
            if      (st & CDIS_SELECTED) bg = kTbBgPressed;
            else if (st & CDIS_HOT)      bg = kTbBgHover;
            HBRUSH br = ::CreateSolidBrush(bg);
            ::FillRect(hdc, &rc, br);
            ::DeleteObject(br);
            const wchar_t* label = (id == kIdGrid3)   ? L"3×3"
                                 : (id == kIdGrid5)   ? L"5×5"
                                 : (id == kIdVisual)  ? L"Visuel"
                                 : (id == kIdInspect) ? L"Inspecté" : L"";
            HFONT old = static_cast<HFONT>(::SelectObject(hdc, bar_font_));
            COLORREF fg = ((st & CDIS_HOT) || (st & CDIS_SELECTED) || (st & CDIS_CHECKED))
                        ? kTbAccent : kTbFg;
            ::SetTextColor(hdc, fg);
            ::SetBkMode(hdc, TRANSPARENT);
            ::DrawTextW(hdc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject(hdc, old);
            if (st & CDIS_CHECKED) {
                RECT s = rc; s.top = s.bottom - 2;
                HBRUSH ab = ::CreateSolidBrush(kTbAccent);
                ::FillRect(hdc, &s, ab);
                ::DeleteObject(ab);
            }
            return CDRF_SKIPDEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

void AberrationWindow::destroy() {
    release_d2d();
    safe_release(text_);
    safe_release(dwrite_factory_);
    if (bar_font_) { ::DeleteObject(bar_font_); bar_font_ = nullptr; }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void AberrationWindow::show() { if (hwnd_) ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE); }
void AberrationWindow::hide() { if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE); }

// Initial window sizing only (the window is freely resizable afterwards).
void AberrationWindow::resize_to_grid() {
    if (!hwnd_) return;
    const int gridpx = grid_ * kBaseTile + (grid_ + 1) * kGap;
    RECT wr = { 0, 0, gridpx, gridpx + kBarH };
    ::AdjustWindowRect(&wr, static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void AberrationWindow::set_grid(int g) {
    if (bar_) {
        ::SendMessageW(bar_, TB_CHECKBUTTON, kIdGrid3, MAKELPARAM(g == 3, 0));
        ::SendMessageW(bar_, TB_CHECKBUTTON, kIdGrid5, MAKELPARAM(g == 5, 0));
    }
    if (g != 3 && g != 5) return;
    if (g == grid_) return;
    grid_ = g;
    // Keep the user's window size; tiles re-derive from the client area.
    release_tiles();
    crops_.clear(); have_crops_ = false;
    plate_for_ = nullptr;          // force a plate rebuild at the new grid
    if (on_layout_changed_) on_layout_changed_();
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::set_mode(Mode m) {
    if (bar_) {
        ::SendMessageW(bar_, TB_CHECKBUTTON, kIdVisual,  MAKELPARAM(m == Mode::Visual, 0));
        ::SendMessageW(bar_, TB_CHECKBUTTON, kIdInspect, MAKELPARAM(m == Mode::Inspected, 0));
    }
    if (m == mode_) return;
    mode_ = m;
    if (on_layout_changed_) on_layout_changed_();
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == rot_) return;
    rot_ = deg;
    have_crops_ = false;      // visual re-extracts on the next set_source
    // The PSF plate (star detection) is rotation-independent -- only the display
    // stamps rotate. If the cached plate is still valid for this image/grid, keep
    // it and just re-rotate the stamps (milliseconds) instead of re-running the
    // multi-second detection. plate_for_ is left set so the set_image() the
    // viewer sends right after sees a cache hit and skips the recompute.
    if (plate_.success && img_ && plate_for_ == img_.get() && plate_grid_ == grid_) {
        rebuild_inspected_display();
    } else {
        plate_for_ = nullptr;   // no valid cached plate -> full rebuild later
    }
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::clear() {
    release_tiles();
    crops_.clear(); have_crops_ = false;
    for (auto& z : dzones_) for (auto& s : z.stars) safe_release(s.tex);
    dzones_.clear(); have_plate_ = false; plate_for_ = nullptr;
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::set_source(const fitsx::RenderedBitmap& rb) {
    if (mode_ != Mode::Visual || !is_visible()) return;
    build_visual(rb);
}

void AberrationWindow::set_image(std::shared_ptr<const fitsx::FitsImage> img) {
    img_ = std::move(img);
    if (mode_ != Mode::Inspected || !is_visible()) return;
    // Recompute only when the image (or grid) actually changed.
    if (img_.get() == plate_for_ && grid_ == plate_grid_ && have_plate_) return;
    build_inspected();
}

void AberrationWindow::build_visual(const fitsx::RenderedBitmap& rb) {
    release_tiles();
    crops_.clear(); have_crops_ = false;
    if (rb.width <= 0 || rb.height <= 0 || rb.bgra.empty()) {
        if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const int N = grid_, W = rb.width, H = rb.height;
    // Crop a region roughly the size of the on-screen tile (so it's ~1:1), but
    // capped so a maximised window still has crisp pixels to magnify.
    int C = std::clamp(tile_px(), 48, 320);
    C = std::min({ C, W / N, H / N });
    if (C < 16) C = std::min({ W, H, 16 });
    if (C < 1) { if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE); return; }
    crops_.assign(static_cast<size_t>(N) * N, Crop{});
    tiles_.assign(static_cast<size_t>(N) * N, nullptr);
    auto axis_centre = [](int i, int n, int L, int c) {
        return (n <= 1) ? L / 2 : c / 2 + i * (L - c) / (n - 1);
    };
    std::vector<uint8_t> raw;
    for (int dr = 0; dr < N; ++dr)        // displayed cell
        for (int dc = 0; dc < N; ++dc) {
            int sr, sc;                    // -> source cell under rotation
            disp_to_src_cell(dr, dc, N, rot_, sr, sc);
            int x0 = std::clamp(axis_centre(sc, N, W, C) - C / 2, 0, std::max(0, W - C));
            int y0 = std::clamp(axis_centre(sr, N, H, C) - C / 2, 0, std::max(0, H - C));
            blit_crop(rb, x0, y0, C, raw);
            Crop& cr = crops_[static_cast<size_t>(dr) * N + dc];
            cr.w = C; cr.h = C;
            rotate_bgra_square(raw, C, rot_, cr.bgra);   // rotate to displayed orientation
        }
    have_crops_ = true;
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::build_inspected() {
    // Expensive half: run the PSF plate (detection + moments) and cache it, so a
    // later rotation only re-rotates the display stamps (see set_rotation).
    if (!img_ || img_->empty()) {
        for (auto& z : dzones_) for (auto& s : z.stars) safe_release(s.tex);
        dzones_.clear(); have_plate_ = false; plate_ = {};
        plate_for_ = nullptr;
        if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    plate_ = fitsx::compute_psf_plate(*img_, grid_, 4);
    plate_for_ = img_.get(); plate_grid_ = grid_;
    rebuild_inspected_display();
}

void AberrationWindow::rebuild_inspected_display() {
    // Cheap half: turn the cached plate into rotated, displayed-order stamps.
    // Re-runnable on rotation without re-detecting stars.
    for (auto& z : dzones_) for (auto& s : z.stars) safe_release(s.tex);
    dzones_.clear(); have_plate_ = false;
    if (!img_ || !plate_.success ||
        static_cast<int>(plate_.zones.size()) != grid_ * grid_) {
        if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const int N = grid_;
    const float rot_rad = rot_ * 3.14159265f / 180.0f;
    dzones_.assign(static_cast<size_t>(N) * N, DispZone{});
    std::vector<uint8_t> raw;
    for (int dr = 0; dr < N; ++dr)            // displayed zone
        for (int dc = 0; dc < N; ++dc) {
            int sr, sc;
            disp_to_src_cell(dr, dc, N, rot_, sr, sc);
            const fitsx::PsfZone& z = plate_.zones[static_cast<size_t>(sr) * N + sc];
            DispZone& dz = dzones_[static_cast<size_t>(dr) * N + dc];
            dz.elong = z.elong_median; dz.ecc = z.ecc_median; dz.axis = z.axis;
            for (const auto& star : z.stars) {
                DispStar ds;
                asinh_stamp(*img_, star.ox, star.oy, fitsx::kPsfStamp, raw);
                rotate_bgra_square(raw, fitsx::kPsfStamp, rot_, ds.bgra);
                rotate_pt(star.x0, star.y0, fitsx::kPsfStamp, rot_, ds.x0, ds.y0);
                ds.pa = star.pa + rot_rad;
                ds.siga = star.siga; ds.sigb = star.sigb;
                dz.stars.push_back(std::move(ds));
            }
        }
    have_plate_ = true;
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void AberrationWindow::init_d2d() {
    if (!d2d_factory_)
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (!dwrite_factory_)
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (dwrite_factory_ && !text_)
        dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &text_);
    if (!d2d_factory_ || !hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    d2d_factory_->CreateHwndRenderTarget(props, hprops, &rt_);
    if (!rt_) return;
    rt_->CreateSolidColorBrush(kPanelBg, &br_panel_);
    rt_->CreateSolidColorBrush(kFrame,   &br_frame_);
    rt_->CreateSolidColorBrush(kAccent,  &br_accent_);
}

void AberrationWindow::release_tiles() {
    for (auto& t : tiles_) safe_release(t);
}

void AberrationWindow::release_d2d() {
    release_tiles();
    for (auto& z : dzones_) for (auto& s : z.stars) safe_release(s.tex);
    safe_release(br_panel_);
    safe_release(br_frame_);
    safe_release(br_accent_);
    safe_release(rt_);
}

void AberrationWindow::render() {
    if (!rt_) { init_d2d(); if (!rt_) return; }
    rt_->BeginDraw();
    rt_->Clear(kPanelBg);
    if (mode_ == Mode::Visual) render_visual();
    else                       render_inspected();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) release_d2d();
}

void AberrationWindow::render_visual() {
    const int N = grid_, tile = tile_px();
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const int centre = N / 2;
    for (int row = 0; row < N; ++row)
        for (int col = 0; col < N; ++col) {
            const int idx = row * N + col;
            const float tx = static_cast<float>(kGap + col * (tile + kGap));
            const float ty = static_cast<float>(kBarH + kGap + row * (tile + kGap));
            const D2D1_RECT_F tilerc = D2D1::RectF(tx, ty, tx + tile, ty + tile);
            if (have_crops_ && idx < static_cast<int>(crops_.size())) {
                Crop& cr = crops_[static_cast<size_t>(idx)];
                if (cr.w > 0) {
                    if (!tiles_[static_cast<size_t>(idx)])
                        rt_->CreateBitmap(D2D1::SizeU(cr.w, cr.h), cr.bgra.data(), cr.w * 4, bp,
                                          &tiles_[static_cast<size_t>(idx)]);
                    if (auto* bmp = tiles_[static_cast<size_t>(idx)])
                        // Scale the crop to fill the tile (nearest = honest
                        // magnified pixels) so a bigger window shows it bigger.
                        rt_->DrawBitmap(bmp, tilerc, 1.0f,
                                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                }
            }
            const bool is_center = (row == centre && col == centre);
            rt_->DrawRectangle(tilerc, is_center ? br_accent_ : br_frame_, 1.0f);
        }
    if (mode_ == Mode::Visual) {
        const float cx = kGap + centre * (tile + kGap) + tile * 0.5f;
        const float cy = kBarH + kGap + centre * (tile + kGap) + tile * 0.5f;
        br_accent_->SetOpacity(0.5f);
        rt_->DrawLine(D2D1::Point2F(cx - 7, cy), D2D1::Point2F(cx + 7, cy), br_accent_, 1.0f);
        rt_->DrawLine(D2D1::Point2F(cx, cy - 7), D2D1::Point2F(cx, cy + 7), br_accent_, 1.0f);
        br_accent_->SetOpacity(1.0f);
    }
}

void AberrationWindow::render_inspected() {
    const int N = grid_, tile = tile_px();
    if (!have_plate_ || static_cast<int>(dzones_.size()) != N * N) {
        if (text_) {
            br_accent_->SetColor(kFrame);
            RECT rc; ::GetClientRect(hwnd_, &rc);
            rt_->DrawTextW(L"Analyse PSF…", 12, text_,
                D2D1::RectF(12, kBarH + 12.0f, static_cast<float>(rc.right) - 8, kBarH + 34.0f),
                br_accent_);
            br_accent_->SetColor(kAccent);
        }
        return;
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const int CUT = fitsx::kPsfStamp;
    const float sub = (tile - kSubGap) * 0.5f;     // 2x2 sub-stamps per cell
    const float mag = sub / CUT;
    const int centre = N / 2;

    for (int gy = 0; gy < N; ++gy)
        for (int gx = 0; gx < N; ++gx) {
            DispZone& zone = dzones_[static_cast<size_t>(gy) * N + gx];
            const float cellx = static_cast<float>(kGap + gx * (tile + kGap));
            const float celly = static_cast<float>(kBarH + kGap + gy * (tile + kGap));

            for (int slot = 0; slot < 4; ++slot) {
                const float sxp = cellx + (slot % 2) * (sub + kSubGap);
                const float syp = celly + (slot / 2) * (sub + kSubGap);
                const D2D1_RECT_F sr = D2D1::RectF(sxp, syp, sxp + sub, syp + sub);
                if (slot < static_cast<int>(zone.stars.size())) {
                    DispStar& st = zone.stars[static_cast<size_t>(slot)];
                    if (!st.tex && !st.bgra.empty())
                        rt_->CreateBitmap(D2D1::SizeU(CUT, CUT), st.bgra.data(), CUT * 4, bp, &st.tex);
                    if (st.tex)
                        rt_->DrawBitmap(st.tex, sr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    // 2.5-sigma ellipse + red centroid cross (already rotated).
                    const float ex = sxp + st.x0 * mag, ey = syp + st.y0 * mag;
                    br_accent_->SetColor(kEllipse);
                    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(st.pa * 57.29578f,
                                                                 D2D1::Point2F(ex, ey)));
                    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ex, ey),
                        std::max(1.5f, 2.5f * st.siga * mag), std::max(1.5f, 2.5f * st.sigb * mag)),
                        br_accent_, 1.4f);
                    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
                    br_accent_->SetColor(kCross);
                    rt_->DrawLine(D2D1::Point2F(ex - 4, ey), D2D1::Point2F(ex + 4, ey), br_accent_, 1.0f);
                    rt_->DrawLine(D2D1::Point2F(ex, ey - 4), D2D1::Point2F(ex, ey + 4), br_accent_, 1.0f);
                    br_accent_->SetColor(kAccent);
                } else {
                    br_accent_->SetColor(D2D1::ColorF(0x161a20));
                    rt_->FillRectangle(sr, br_accent_);
                    br_accent_->SetColor(kAccent);
                }
                rt_->DrawRectangle(sr, br_frame_, 1.0f);
            }

            // Zone label (elong / ecc / axis class) at the cell bottom.
            if (text_ && !zone.stars.empty()) {
                wchar_t v[48];
                const int n = swprintf_s(v, L"e%.2f  c%.2f  %s",
                                         zone.elong, zone.ecc, axis_str(zone.axis));
                br_accent_->SetColor(D2D1::ColorF(0xF0E882));
                if (n > 0)
                    rt_->DrawTextW(v, static_cast<UINT32>(n), text_,
                        D2D1::RectF(cellx + 3, celly + tile - 17, cellx + tile - 2, celly + tile - 1),
                        br_accent_);
                br_accent_->SetColor(kAccent);
            }

            const bool is_center = (gx == centre && gy == centre);
            rt_->DrawRectangle(
                D2D1::RectF(cellx - 1, celly - 1, cellx + tile, celly + tile),
                is_center ? br_accent_ : br_frame_, is_center ? 1.6f : 1.0f);
        }
}

void AberrationWindow::on_paint() {
    PAINTSTRUCT ps; ::BeginPaint(hwnd_, &ps); render(); ::EndPaint(hwnd_, &ps);
}

void AberrationWindow::on_size(int cx, int cy) {
    if (bar_) ::MoveWindow(bar_, 0, 0, cx, kBarH, TRUE);
    if (rt_) rt_->Resize(D2D1::SizeU(std::max(1, cx), std::max(1, cy)));
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK AberrationWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<AberrationWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdGrid3:   self->set_grid(3); return 0;
                case kIdGrid5:   self->set_grid(5); return 0;
                case kIdVisual:  self->set_mode(Mode::Visual); return 0;
                case kIdInspect: self->set_mode(Mode::Inspected); return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm && nm->hwndFrom == self->bar_ && nm->code == NM_CUSTOMDRAW)
                return self->on_bar_customdraw(reinterpret_cast<LPNMTBCUSTOMDRAW>(lp));
            break;
        }
        case WM_PAINT:      self->on_paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            self->on_size(LOWORD(lp), HIWORD(lp));
            // Maximise / restore don't emit WM_EXITSIZEMOVE: re-extract crisp now.
            if ((wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED) && self->on_layout_changed_)
                self->on_layout_changed_();
            return 0;
        case WM_EXITSIZEMOVE:    // end of a drag-resize: re-extract at the new size
            if (self->on_layout_changed_) self->on_layout_changed_();
            return 0;
        case WM_CLOSE:      self->hide(); return 0;
        case WM_DESTROY:    return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
