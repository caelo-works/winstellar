#include "ViewerWindow.h"

#include "fits_core/fits_loader.h"
#include "fits_core/analysis.h"
#include "fits_core/cache.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarMainWindow";
constexpr const wchar_t* kRegKeyPath = L"Software\\WinStellar";

struct WindowPlacement {
    int  x = CW_USEDEFAULT;
    int  y = CW_USEDEFAULT;
    int  w = 1200;
    int  h = 800;
    bool maximized = false;
};

bool reg_read_dword(HKEY k, const wchar_t* name, DWORD* out) {
    DWORD type = 0, sz = sizeof(DWORD);
    return ::RegQueryValueExW(k, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(out), &sz) == ERROR_SUCCESS
           && type == REG_DWORD;
}

bool load_window_placement(WindowPlacement& wp) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        return false;
    }
    DWORD x=0, y=0, w=0, h=0, mx=0;
    const bool ok = reg_read_dword(k, L"WindowX", &x)
                 && reg_read_dword(k, L"WindowY", &y)
                 && reg_read_dword(k, L"WindowWidth",  &w)
                 && reg_read_dword(k, L"WindowHeight", &h);
    reg_read_dword(k, L"WindowMaximized", &mx);   // optional
    ::RegCloseKey(k);
    if (!ok || w < 200 || h < 150) return false;

    // Verify the rect is actually on a connected monitor — fall back to
    // defaults if the user unplugged the display the window was last on.
    RECT rc = { static_cast<LONG>(x), static_cast<LONG>(y),
                static_cast<LONG>(x + w), static_cast<LONG>(y + h) };
    if (::MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) == nullptr) return false;

    wp.x = static_cast<int>(x);
    wp.y = static_cast<int>(y);
    wp.w = static_cast<int>(w);
    wp.h = static_cast<int>(h);
    wp.maximized = (mx != 0);
    return true;
}

void save_window_placement(HWND hwnd) {
    if (!hwnd) return;
    // GetWindowPlacement gives the **restored** (un-maximized) rect even if
    // the window is currently maximized — exactly what we want to persist.
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    const DWORD x  = static_cast<DWORD>(r.left);
    const DWORD y  = static_cast<DWORD>(r.top);
    const DWORD w  = static_cast<DWORD>(r.right - r.left);
    const DWORD h  = static_cast<DWORD>(r.bottom - r.top);
    const DWORD mx = (wp.showCmd == SW_SHOWMAXIMIZED) ? 1u : 0u;

    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                          &k, nullptr) != ERROR_SUCCESS) return;
    auto put = [&](const wchar_t* name, DWORD v) {
        ::RegSetValueExW(k, name, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    put(L"WindowX", x);
    put(L"WindowY", y);
    put(L"WindowWidth",  w);
    put(L"WindowHeight", h);
    put(L"WindowMaximized", mx);
    ::RegCloseKey(k);
}
constexpr int kHeaderPanelWidth = 320;
// App-wide dark theme color (#0b0d10). Used for the DWM title bar, the D2D
// viewport clear, and the listview backgrounds so the chrome blends in.
constexpr COLORREF kThemeBgColorRef = RGB(0x0b, 0x0d, 0x10);

// Accent color used for column-header text — same cyan as the app icon.
constexpr COLORREF kAccentColorRef = RGB(0x7d, 0xd3, 0xfc);

HFONT get_bold_ui_font(HWND ref) {
    static HFONT cached = nullptr;
    if (cached) return cached;
    HFONT base = reinterpret_cast<HFONT>(::SendMessageW(ref, WM_GETFONT, 0, 0));
    if (!base) base = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    LOGFONTW lf{};
    if (::GetObjectW(base, sizeof(lf), &lf)) {
        lf.lfWeight = FW_BOLD;
        cached = ::CreateFontIndirectW(&lf);
    }
    return cached;
}

// Subclass on each listview that intercepts the header child's custom-draw
// notifications (the header sends NM_CUSTOMDRAW to its parent — the listview —
// not to our top-level window, which is why we have to peek at it here).
LRESULT CALLBACK listview_subproc(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR /*ref*/) {
    if (m == WM_NOTIFY) {
        auto* hdr = reinterpret_cast<LPNMHDR>(lp);
        if (hdr && hdr->code == NM_CUSTOMDRAW &&
            hdr->hwndFrom == ListView_GetHeader(h)) {
            auto* cd = reinterpret_cast<LPNMCUSTOMDRAW>(lp);
            switch (cd->dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    if (HFONT bold = get_bold_ui_font(hdr->hwndFrom))
                        ::SelectObject(cd->hdc, bold);
                    ::SetTextColor(cd->hdc, kAccentColorRef);
                    ::SetBkMode(cd->hdc, TRANSPARENT);
                    return CDRF_NEWFONT;
            }
        }
    } else if (m == WM_NCDESTROY) {
        ::RemoveWindowSubclass(h, listview_subproc, id);
    }
    return ::DefSubclassProc(h, m, wp, lp);
}

void apply_dark_listview(HWND lv) {
    if (!lv) return;
    // Body + text. SetTextBkColor must match SetBkColor or text rendering
    // gets a light-grey halo around each glyph.
    ListView_SetBkColor    (lv, kThemeBgColorRef);
    ListView_SetTextBkColor(lv, kThemeBgColorRef);
    ListView_SetTextColor  (lv, RGB(0xE0, 0xE2, 0xE5));
    // Theme the column header + scrollbar via the (undocumented but stable
    // since Win10 1809) DarkMode_Explorer visual style.
    ::SetWindowTheme(lv, L"DarkMode_Explorer", nullptr);
    HWND header = ListView_GetHeader(lv);
    if (header) ::SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
    // Bold + accent-colored column-header text via NM_CUSTOMDRAW.
    ::SetWindowSubclass(lv, listview_subproc, /*id=*/1, /*ref=*/0);
}

void apply_dark_titlebar(HWND hwnd) {
    // Caption buttons (close/min/max) follow the system's dark theme.
    // Attribute index 20 since Windows 10 1903+; older builds ignore it.
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, /*DWMWA_USE_IMMERSIVE_DARK_MODE*/ 20,
                            &dark, sizeof(dark));
    // Custom caption bg + border (Windows 11 22H2+; ignored on older builds).
    COLORREF bg = kThemeBgColorRef;
    ::DwmSetWindowAttribute(hwnd, /*DWMWA_CAPTION_COLOR*/ 35, &bg, sizeof(bg));
    ::DwmSetWindowAttribute(hwnd, /*DWMWA_BORDER_COLOR*/ 34, &bg, sizeof(bg));
    COLORREF text = RGB(0xE8, 0xEA, 0xED);
    ::DwmSetWindowAttribute(hwnd, /*DWMWA_TEXT_COLOR*/ 36, &text, sizeof(text));
}
constexpr int kIdHeaderList = 1001;
constexpr int kIdAnalysisList = 1002;

constexpr int kCmd_Open = 100;
constexpr int kCmd_ToggleHeaders = 101;
constexpr int kCmd_FitToWindow = 102;
constexpr int kCmd_Zoom100 = 103;
constexpr int kCmd_ZoomIn = 104;
constexpr int kCmd_ZoomOut = 105;
constexpr int kCmd_ToggleAnalysis = 106;
constexpr int kCmd_RotateLeft     = 107;
constexpr int kCmd_RotateRight    = 108;
constexpr int kCmd_StretchNone    = 109;
constexpr int kCmd_StretchAuto    = 110;
constexpr int kCmd_PrevFile       = 111;
constexpr int kCmd_NextFile       = 112;

// Worker → UI postback. wParam = generation, lParam = ViewerWindow::LoadResult*.
constexpr UINT  WM_APP_LOAD_DONE = WM_APP + 1;
// Spinner animation timer (~60 Hz). Only running while loading_ is true.
constexpr UINT_PTR kTimerSpinner = 1;
constexpr float    kSpinnerRadius = 24.0f;  // dot orbit radius, in DIPs

template <typename T>
void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool has_astro_extension(const wchar_t* name) {
    const wchar_t* ext = ::PathFindExtensionW(name);
    if (!ext || !*ext) return false;
    return ::_wcsicmp(ext, L".fit")  == 0
        || ::_wcsicmp(ext, L".fits") == 0
        || ::_wcsicmp(ext, L".xisf") == 0;
}

// Enumerate sibling astro files in the directory of `path`, natural-sorted
// (so img2 < img10) and case-insensitive. Returns empty on failure; the
// current file itself is included so callers can locate it by name.
std::vector<std::wstring> enumerate_astro_siblings(const std::wstring& path) {
    std::vector<std::wstring> out;
    if (path.empty()) return out;

    wchar_t dir[MAX_PATH] = {};
    if (wcscpy_s(dir, MAX_PATH, path.c_str()) != 0) return out;
    if (!::PathRemoveFileSpecW(dir)) return out;

    wchar_t pattern[MAX_PATH] = {};
    if (wcscpy_s(pattern, MAX_PATH, dir) != 0) return out;
    if (!::PathAppendW(pattern, L"*"))   return out;

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW(pattern, FindExInfoBasic, &fd,
                                  FindExSearchNameMatch, nullptr,
                                  FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_astro_extension(fd.cFileName)) continue;
        out.emplace_back(fd.cFileName);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    std::sort(out.begin(), out.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return ::StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
              });
    return out;
}

}  // namespace

bool ViewerWindow::create(HINSTANCE hinst, const wchar_t* initial_path) {
    hinst_ = hinst;
    init_d2d_factory();
    if (!d2d_factory_) return false;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // we handle WM_ERASEBKGND
    wc.lpszClassName = kClassName;
    // Pull resource ID 1 from the .exe at the right pixel size for each slot;
    // LoadIconW alone always hands back 32x32 and Windows then resamples it,
    // which looks fuzzy in the title bar at high DPI.
    wc.hIcon = static_cast<HICON>(::LoadImageW(
        hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(
        hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!::RegisterClassExW(&wc)) return false;

    WindowPlacement wp{};
    const bool restored = load_window_placement(wp);

    // WS_CLIPCHILDREN: WM_PAINT region excludes child windows (the listview),
    // so panning/zooming doesn't repaint the headers and they stop flickering.
    hwnd_ = ::CreateWindowExW(
        WS_EX_ACCEPTFILES, kClassName, L"WinStellar",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        wp.x, wp.y, wp.w, wp.h,
        nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;
    // ShowWindow happens later; remember whether to restore as maximized.
    const bool start_maximized = restored && wp.maximized;

    apply_dark_titlebar(hwnd_);

    // Ensure system common controls created next inherit the parent's
    // per-monitor DPI awareness; without this, ToolbarWindow32 / SysListView32
    // get their own (often DPI_UNAWARE) context and end up scaled by the
    // inverse DPI ratio (e.g. 320 logical → 256 physical at 125 % zoom),
    // collapsing the sidebar into a narrow strip on the right edge.
    DPI_AWARENESS_CONTEXT prev_dpi_ctx = ::SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    toolbar_.create(hwnd_, hinst);
    toolbar_.set_analysis_active(true);  // both panels visible by default
    toolbar_.set_headers_active (true);
    toolbar_.set_stretch_auto_active(true);  // default = auto stretch
    toolbar_.set_stretch_none_active(false);
    toolbar_.set_nav_enabled(false);         // no file yet → Prev/Next greyed

    analysis_.create(hwnd_, hinst, kIdAnalysisList);
    headers_.create(hwnd_, hinst, kIdHeaderList);
    apply_dark_listview(analysis_.hwnd());
    apply_dark_listview(headers_.hwnd());

    if (prev_dpi_ctx) ::SetThreadDpiAwarenessContext(prev_dpi_ctx);

    // Global keyboard shortcuts via an accelerator table — works even when the
    // listview has focus (TranslateAcceleratorW intercepts before WM_KEYDOWN
    // reaches the focus window).
    ACCEL accel_entries[] = {
        {FCONTROL | FVIRTKEY,           'O',         kCmd_Open},
        {FVIRTKEY,                       'A',         kCmd_ToggleAnalysis},
        {FVIRTKEY,                       'H',         kCmd_ToggleHeaders},
        {FVIRTKEY,                       'F',         kCmd_FitToWindow},
        {FVIRTKEY,                       '1',         kCmd_Zoom100},
        {FVIRTKEY,                       VK_OEM_PLUS, kCmd_ZoomIn},
        {FVIRTKEY,                       VK_ADD,      kCmd_ZoomIn},
        {FVIRTKEY,                       VK_OEM_MINUS,kCmd_ZoomOut},
        {FVIRTKEY,                       VK_SUBTRACT, kCmd_ZoomOut},
        {FVIRTKEY,                       'R',         kCmd_RotateRight},
        {FSHIFT | FVIRTKEY,              'R',         kCmd_RotateLeft},
        {FVIRTKEY,                       VK_LEFT,     kCmd_PrevFile},
        {FVIRTKEY,                       VK_RIGHT,    kCmd_NextFile},
        {FVIRTKEY,                       VK_PRIOR,    kCmd_PrevFile},
        {FVIRTKEY,                       VK_NEXT,     kCmd_NextFile},
    };
    accel_ = ::CreateAcceleratorTableW(accel_entries, _countof(accel_entries));

    ::ShowWindow(hwnd_, start_maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    ::UpdateWindow(hwnd_);
    ::SetFocus(hwnd_);

    if (initial_path && *initial_path) load_file(initial_path);
    return true;
}

int ViewerWindow::run_message_loop() {
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (accel_ && ::TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    if (accel_) { ::DestroyAcceleratorTable(accel_); accel_ = nullptr; }
    release_render_target();
    safe_release(d2d_factory_);
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK ViewerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ViewerWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        auto* w = static_cast<ViewerWindow*>(cs->lpCreateParams);
        if (w) w->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_SIZE:
            self->on_size(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_PAINT:
            self->on_paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEWHEEL: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(hwnd, &pt);
            self->on_wheel(GET_WHEEL_DELTA_WPARAM(wp), pt.x, pt.y);
            return 0;
        }
        case WM_LBUTTONDOWN: self->on_lbutton_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEMOVE:   self->on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP:   self->on_lbutton_up(); return 0;
        case WM_KEYDOWN:     self->on_keydown(wp); return 0;
        case WM_DROPFILES:   self->on_drop(reinterpret_cast<HDROP>(wp)); return 0;
        case WM_COMMAND:     self->on_command(LOWORD(wp)); return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm && nm->hwndFrom == self->headers_.hwnd() && nm->code == LVN_KEYDOWN) {
                auto* kd = reinterpret_cast<NMLVKEYDOWN*>(nm);
                if (kd->wVKey == 'C' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
                    self->headers_.copy_selection_to_clipboard();
                    return 0;
                }
            }
            // Toolbar custom-draw + tooltip routing.
            if (nm && nm->hwndFrom == self->toolbar_.hwnd() && nm->code == NM_CUSTOMDRAW) {
                return self->toolbar_.on_customdraw(reinterpret_cast<LPNMTBCUSTOMDRAW>(lp));
            }
            if (nm && nm->code == TTN_GETDISPINFOW) {
                self->toolbar_.on_tooltip(reinterpret_cast<LPNMTTDISPINFOW>(lp));
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (wp == kTimerSpinner) {
                // Only invalidate a small box around the spinner — the dark
                // veil and underlying bitmap haven't changed since the prior
                // frame, so repainting the full viewport every 16 ms would
                // burn GPU on huge bitmaps.
                const RECT vp = self->viewport_rect();
                const int cx = (vp.left + vp.right) / 2;
                const int cy = (vp.top  + vp.bottom) / 2;
                const int pad = static_cast<int>(kSpinnerRadius) + 12;
                RECT spin = { cx - pad, cy - pad, cx + pad, cy + pad };
                ::InvalidateRect(hwnd, &spin, FALSE);
                return 0;
            }
            break;
        case WM_APP_LOAD_DONE:
            self->on_load_finished(static_cast<std::uint64_t>(wp),
                                   reinterpret_cast<LoadResult*>(lp));
            return 0;
        case WM_DESTROY:
            save_window_placement(hwnd);
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void ViewerWindow::init_d2d_factory() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
}

void ViewerWindow::create_render_target() {
    release_render_target();
    if (!d2d_factory_ || !hwnd_) return;
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    const int rt_w = std::max<LONG>(1, rc.right);
    const int rt_h = std::max<LONG>(1, rc.bottom);

    // Force the render target to 96 DPI so our coordinates are interpreted
    // as raw pixels (1:1 with GetClientRect, which already returns physical
    // pixels for our PerMonitorV2-aware process). Without this, D2D applies
    // its own DPI scaling on top of the per-monitor awareness, doubling the
    // scale factor — at 125 % display zoom that pushed the image past the
    // bottom of the render target.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(),
        96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(rt_w, rt_h));
    d2d_factory_->CreateHwndRenderTarget(props, hprops, &rt_);
}

void ViewerWindow::release_render_target() {
    safe_release(bitmap_);
    safe_release(rt_);
}

bool ViewerWindow::ensure_d2d_bitmap() {
    if (!rt_ || rendered_.width <= 0 || rendered_.height <= 0) return false;
    if (bitmap_) return true;

    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    return SUCCEEDED(rt_->CreateBitmap(
        D2D1::SizeU(rendered_.width, rendered_.height),
        rendered_.bgra.data(), rendered_.stride_bytes, bp, &bitmap_));
}

RECT ViewerWindow::viewport_rect() const {
    RECT rc{};
    if (!hwnd_) return rc;
    ::GetClientRect(hwnd_, &rc);
    rc.top = std::min<LONG>(rc.bottom, rc.top + Toolbar::kHeight);
    if (analysis_.visible() || headers_.visible()) {
        rc.right = std::max<LONG>(0, rc.right - kHeaderPanelWidth);
    }
    return rc;
}

void ViewerWindow::invalidate_viewport() {
    if (!hwnd_) return;
    RECT vp = viewport_rect();
    ::InvalidateRect(hwnd_, &vp, FALSE);
}

void ViewerWindow::update_title() {
    wchar_t title[1280] = {};
    const int zoom_pct = static_cast<int>(zoom_ * 100.0f + 0.5f);
    if (!loaded_path_.empty()) {
        swprintf_s(title, L"WinStellar — %s — %d%%", loaded_path_.c_str(), zoom_pct);
    } else {
        swprintf_s(title, L"WinStellar — %d%%", zoom_pct);
    }
    ::SetWindowTextW(hwnd_, title);
}

void ViewerWindow::apply_stretch() {
    if (image_.data.empty()) return;
    stretch_ = (stretch_mode_ == StretchMode::Auto)
        ? fitsx::compute_auto_stretch(image_)
        : fitsx::StretchParams{};   // identity = no stretch
    rendered_ = fitsx::render_to_bgra(image_, stretch_);
    safe_release(bitmap_);
}

// Heap-allocated bundle passed from the worker thread back to the UI thread
// via PostMessage. Owns large buffers (FitsImage, RenderedBitmap) — the UI
// move-steals them out before destroying it.
struct ViewerWindow::LoadResult {
    bool                  success = false;
    std::string           error;
    std::wstring          path;
    fitsx::FitsImage      image;
    fitsx::StretchParams  stretch;
    fitsx::RenderedBitmap rendered;
    fitsx::AnalysisResult analysis;
};

void ViewerWindow::load_file(const wchar_t* path) {
    if (!path || !*path) return;

    const std::uint64_t gen = load_gen_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Show the spinner immediately — the worker may take seconds for big XISFs.
    loading_ = true;
    if (const wchar_t* name = ::PathFindFileNameW(path)) {
        loading_filename_ = name;
    } else {
        loading_filename_ = path;
    }
    ::SetTimer(hwnd_, kTimerSpinner, 16, nullptr);
    invalidate_viewport();

    const HWND        hwnd_target = hwnd_;
    const std::wstring path_copy(path);
    const StretchMode mode = stretch_mode_;

    std::thread([hwnd_target, gen, path_copy, mode]() {
        auto result = std::make_unique<LoadResult>();
        result->path = path_copy;

        auto loaded = fitsx::load_from_file(path_copy.c_str());
        if (!loaded.success) {
            result->success = false;
            result->error   = std::move(loaded.error);
            ::PostMessageW(hwnd_target, WM_APP_LOAD_DONE,
                           static_cast<WPARAM>(gen),
                           reinterpret_cast<LPARAM>(result.release()));
            return;
        }
        result->image    = std::move(loaded.image);
        result->stretch  = (mode == StretchMode::Auto)
                           ? fitsx::compute_auto_stretch(result->image)
                           : fitsx::StretchParams{};
        result->rendered = fitsx::render_to_bgra(result->image, result->stretch);

        const std::string ckey = fitsx::compute_cache_key_from_file(path_copy.c_str());
        if (!ckey.empty()) {
            if (auto cached = fitsx::AnalysisCache::instance().lookup(ckey); cached.has_value()) {
                result->analysis = *cached;
            } else {
                result->analysis = fitsx::run_analysis(result->image);
                fitsx::AnalysisCache::instance().store(ckey, result->analysis);
            }
        } else {
            result->analysis = fitsx::run_analysis(result->image);
        }
        result->success = true;

        ::PostMessageW(hwnd_target, WM_APP_LOAD_DONE,
                       static_cast<WPARAM>(gen),
                       reinterpret_cast<LPARAM>(result.release()));
    }).detach();
}

void ViewerWindow::on_load_finished(std::uint64_t gen, LoadResult* raw) {
    std::unique_ptr<LoadResult> r(raw);
    // Drop stale results (user spammed Prev/Next while a slow load was in flight).
    if (gen != load_gen_.load(std::memory_order_relaxed)) return;
    // Window may already be tearing down — PostMessage races with WM_DESTROY.
    if (!hwnd_ || !::IsWindow(hwnd_)) return;

    loading_ = false;
    loading_filename_.clear();
    ::KillTimer(hwnd_, kTimerSpinner);

    if (!r->success) {
        wchar_t msg[512] = {};
        swprintf_s(msg, L"Could not open FITS file:\n%hs", r->error.c_str());
        ::MessageBoxW(hwnd_, msg, L"WinStellar", MB_OK | MB_ICONWARNING);
        invalidate_viewport();
        return;
    }

    image_       = std::move(r->image);
    stretch_     = r->stretch;
    rendered_    = std::move(r->rendered);
    loaded_path_ = std::move(r->path);
    // Bitmap was created from the old rendered_ buffer — rebuild on next render.
    safe_release(bitmap_);

    headers_.update(image_.headers);
    analysis_.update(r->analysis);

    zoom_to_fit();
    update_title();
    invalidate_viewport();
    toolbar_.set_nav_enabled(true);
}

void ViewerWindow::navigate_sibling(int step) {
    if (loaded_path_.empty() || step == 0) return;
    auto siblings = enumerate_astro_siblings(loaded_path_);
    if (siblings.empty()) return;

    const wchar_t* current_name = ::PathFindFileNameW(loaded_path_.c_str());
    int idx = -1;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (::_wcsicmp(siblings[i].c_str(), current_name) == 0) {
            idx = static_cast<int>(i);
            break;
        }
    }
    // Current file isn't in the list (rare — e.g. extension we don't enumerate
    // anymore, or it just got deleted). Treat as "start before the first".
    const int n = static_cast<int>(siblings.size());
    if (idx < 0) idx = (step > 0) ? -1 : 0;

    const int next_idx = ((idx + step) % n + n) % n;  // wrap around
    if (next_idx == idx) return;  // only one sibling, no-op

    wchar_t dir[MAX_PATH] = {};
    if (wcscpy_s(dir, MAX_PATH, loaded_path_.c_str()) != 0) return;
    if (!::PathRemoveFileSpecW(dir)) return;

    wchar_t target[MAX_PATH] = {};
    if (wcscpy_s(target, MAX_PATH, dir) != 0) return;
    if (!::PathAppendW(target, siblings[next_idx].c_str())) return;

    load_file(target);
}

void ViewerWindow::layout() {
    if (!hwnd_) return;
    RECT rc;
    ::GetClientRect(hwnd_, &rc);

    toolbar_.resize(0, 0, rc.right);
    const int top_below_toolbar = Toolbar::kHeight;
    const int panel_h = std::max<LONG>(0, rc.bottom - top_below_toolbar);
    const int panel_x = std::max<LONG>(0, rc.right - kHeaderPanelWidth);
    const bool show_a = analysis_.visible();
    const bool show_h = headers_.visible();

    if (show_a && show_h) {
        // Both panels stacked: analysis on top (capped), headers fills the rest.
        const int analysis_h = std::min<LONG>(AnalysisView::kPreferredHeight,
                                              panel_h / 2);
        analysis_.resize(panel_x, top_below_toolbar, kHeaderPanelWidth, analysis_h);
        headers_.resize (panel_x, top_below_toolbar + analysis_h,
                         kHeaderPanelWidth, std::max<LONG>(0, panel_h - analysis_h));
    } else if (show_a) {
        analysis_.resize(panel_x, top_below_toolbar, kHeaderPanelWidth, panel_h);
    } else if (show_h) {
        headers_.resize (panel_x, top_below_toolbar, kHeaderPanelWidth, panel_h);
    }
    if (rt_) {
        rt_->Resize(D2D1::SizeU(std::max<LONG>(1, rc.right),
                                std::max<LONG>(1, rc.bottom)));
    }
    if (rendered_.width > 0) {
        const float minz = min_zoom();
        if (zoom_ < minz) zoom_ = minz;
        clamp_offset();
    }
}

void ViewerWindow::on_size(int /*cx*/, int /*cy*/) {
    if (!rt_) create_render_target();
    layout();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void ViewerWindow::on_paint() {
    PAINTSTRUCT ps;
    ::BeginPaint(hwnd_, &ps);
    if (!rt_) create_render_target();
    if (rt_) render();
    ::EndPaint(hwnd_, &ps);
}

void ViewerWindow::render() {
    if (!rt_) return;
    rt_->BeginDraw();
    // Viewport bg slightly darker than the toolbar/sidebar so the image area
    // is visually distinct.
    rt_->Clear(D2D1::ColorF(0x05070a));

    // Clip drawing to the viewport (left of headers panel) so the listview area
    // stays untouched and the image never bleeds under it.
    const RECT vpr = viewport_rect();
    const D2D1_RECT_F clip = D2D1::RectF(
        static_cast<float>(vpr.left),  static_cast<float>(vpr.top),
        static_cast<float>(vpr.right), static_cast<float>(vpr.bottom));
    rt_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    if (rendered_.width > 0 && rendered_.height > 0 && ensure_d2d_bitmap() && bitmap_) {
        const float img_w = static_cast<float>(rendered_.width);
        const float img_h = static_cast<float>(rendered_.height);
        const float view_w = img_w * zoom_;   // unrotated bitmap-space dims
        const float view_h = img_h * zoom_;

        // When rotated 90 / 270 the image's on-screen bounding box has its
        // axes swapped — center on the swapped bbox, not the unrotated rect.
        const bool sideways = (rotation_deg_ == 90 || rotation_deg_ == 270);
        const float bb_w = sideways ? view_h : view_w;
        const float bb_h = sideways ? view_w : view_h;

        const float vp_w = static_cast<float>(vpr.right - vpr.left);
        const float vp_h = static_cast<float>(vpr.bottom - vpr.top);

        // Center of the (possibly-rotated) image's bounding box in viewport space.
        const float cx = vpr.left + vp_w * 0.5f + (bb_w - bb_w) * 0.0f - offset_x_ * zoom_;
        const float cy = vpr.top  + vp_h * 0.5f                          - offset_y_ * zoom_;

        const float dst_left = cx - view_w * 0.5f;
        const float dst_top  = cy - view_h * 0.5f;
        const D2D1_RECT_F dst = D2D1::RectF(
            dst_left, dst_top, dst_left + view_w, dst_top + view_h);
        const D2D1_RECT_F src = D2D1::RectF(0, 0, img_w, img_h);

        if (rotation_deg_ != 0) {
            rt_->SetTransform(D2D1::Matrix3x2F::Rotation(
                static_cast<float>(rotation_deg_), D2D1::Point2F(cx, cy)));
        }
        rt_->DrawBitmap(bitmap_, dst, 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        if (rotation_deg_ != 0) {
            rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        (void)bb_h;  // computed for symmetry / future use
    }

    // While a load is in flight, dim the viewport and draw an 8-dot spinner
    // over the top. We're still inside the viewport clip from above.
    if (loading_) {
        ID2D1SolidColorBrush* veil_brush = nullptr;
        if (SUCCEEDED(rt_->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &veil_brush))) {
            const D2D1_RECT_F vrc = D2D1::RectF(
                static_cast<float>(vpr.left),  static_cast<float>(vpr.top),
                static_cast<float>(vpr.right), static_cast<float>(vpr.bottom));
            rt_->FillRectangle(vrc, veil_brush);
            veil_brush->Release();
        }

        ID2D1SolidColorBrush* dot_brush = nullptr;
        // Same cyan accent the toolbar uses.
        if (SUCCEEDED(rt_->CreateSolidColorBrush(
                D2D1::ColorF(0x7dd3fc), &dot_brush))) {
            const float cx = (vpr.left + vpr.right) * 0.5f;
            const float cy = (vpr.top  + vpr.bottom) * 0.5f;

            // Lead-dot angle advances ~1.2 turn / second; trailing dots fade.
            const double t = ::GetTickCount64() * 0.001;
            constexpr int n_dots = 8;
            constexpr float two_pi = 6.2831853f;
            const float lead = static_cast<float>(
                std::fmod(t * 1.2 * two_pi, static_cast<double>(two_pi)));

            for (int i = 0; i < n_dots; ++i) {
                const float a = lead - i * (two_pi / n_dots);
                const float dx = kSpinnerRadius * std::cos(a);
                const float dy = kSpinnerRadius * std::sin(a);
                // Lead = full opacity, tail fades to ~15 %.
                const float alpha = 1.0f - (i / static_cast<float>(n_dots - 1)) * 0.85f;
                dot_brush->SetOpacity(alpha);
                rt_->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx + dx, cy + dy), 3.5f, 3.5f),
                    dot_brush);
            }
            dot_brush->Release();
        }
    }

    rt_->PopAxisAlignedClip();

    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        release_render_target();
    }
}

void ViewerWindow::clamp_offset() {
    if (rendered_.width <= 0 || rendered_.height <= 0 || zoom_ <= 0.0f) return;
    const RECT vpr = viewport_rect();
    const float vp_w = static_cast<float>(vpr.right - vpr.left);
    const float vp_h = static_cast<float>(vpr.bottom - vpr.top);
    if (vp_w <= 0 || vp_h <= 0) return;

    // Rotated image bounding box in viewport space.
    const bool sideways = (rotation_deg_ == 90 || rotation_deg_ == 270);
    const float view_w = static_cast<float>(rendered_.width)  * zoom_;
    const float view_h = static_cast<float>(rendered_.height) * zoom_;
    const float bb_w = sideways ? view_h : view_w;
    const float bb_h = sideways ? view_w : view_h;

    // Max screen-space center offset before the image's edge enters the viewport.
    // When the image is smaller than the viewport along an axis, no panning is
    // allowed on that axis (image stays centered).
    const float max_screen_x = std::max(0.0f, (bb_w - vp_w) * 0.5f);
    const float max_screen_y = std::max(0.0f, (bb_h - vp_h) * 0.5f);
    const float max_off_x = max_screen_x / zoom_;
    const float max_off_y = max_screen_y / zoom_;
    offset_x_ = std::clamp(offset_x_, -max_off_x, max_off_x);
    offset_y_ = std::clamp(offset_y_, -max_off_y, max_off_y);
}

float ViewerWindow::min_zoom() const {
    const RECT vpr = viewport_rect();
    const float vp_w = static_cast<float>(vpr.right - vpr.left);
    const float vp_h = static_cast<float>(vpr.bottom - vpr.top);
    if (rendered_.width <= 0 || rendered_.height <= 0 || vp_w <= 0 || vp_h <= 0) {
        return 0.05f;  // safe fallback when nothing is loaded yet
    }
    const bool sideways = (rotation_deg_ == 90 || rotation_deg_ == 270);
    const float fit_w = static_cast<float>(sideways ? rendered_.height : rendered_.width);
    const float fit_h = static_cast<float>(sideways ? rendered_.width  : rendered_.height);
    return std::min(vp_w / fit_w, vp_h / fit_h);
}

void ViewerWindow::zoom_to_fit() {
    if (rendered_.width <= 0 || rendered_.height <= 0) {
        zoom_ = 1.0f;
        offset_x_ = offset_y_ = 0.0f;
        return;
    }
    zoom_ = min_zoom();
    offset_x_ = offset_y_ = 0.0f;
}

void ViewerWindow::set_zoom(float factor, int anchor_x, int anchor_y) {
    if (rendered_.width <= 0) return;
    // Lower bound: never let the user zoom out past the fit-to-window value
    // (below it the image would be smaller than the viewport in both dims —
    // useless screen real-estate).
    const float new_zoom = std::clamp(zoom_ * factor, min_zoom(), 64.0f);

    const RECT vpr = viewport_rect();
    const float vp_cx = (vpr.left + vpr.right) * 0.5f;
    const float vp_cy = (vpr.top  + vpr.bottom) * 0.5f;

    // Image-space coordinate that is currently under the (anchor_x, anchor_y)
    // window-space point. Solve so it stays anchored after the zoom change.
    const float img_x = (anchor_x - vp_cx) / zoom_ + offset_x_ + rendered_.width  * 0.5f;
    const float img_y = (anchor_y - vp_cy) / zoom_ + offset_y_ + rendered_.height * 0.5f;
    zoom_ = new_zoom;
    offset_x_ = img_x - rendered_.width  * 0.5f - (anchor_x - vp_cx) / zoom_;
    offset_y_ = img_y - rendered_.height * 0.5f - (anchor_y - vp_cy) / zoom_;
    clamp_offset();
}

void ViewerWindow::on_wheel(int delta, int x, int y) {
    const float factor = (delta > 0) ? 1.15f : 1.0f / 1.15f;
    set_zoom(factor, x, y);
    update_title();
    invalidate_viewport();
}

void ViewerWindow::on_lbutton_down(int x, int y) {
    // Reclaim focus from the listview so keyboard shortcuts route through us.
    ::SetFocus(hwnd_);
    dragging_ = true;
    drag_start_ = {x, y};
    drag_start_off_x_ = offset_x_;
    drag_start_off_y_ = offset_y_;
    ::SetCapture(hwnd_);
}

void ViewerWindow::on_mouse_move(int x, int y) {
    if (!dragging_ || zoom_ <= 0.0f) return;
    offset_x_ = drag_start_off_x_ - (x - drag_start_.x) / zoom_;
    offset_y_ = drag_start_off_y_ - (y - drag_start_.y) / zoom_;
    clamp_offset();
    invalidate_viewport();
}

void ViewerWindow::on_lbutton_up() {
    if (dragging_) { dragging_ = false; ::ReleaseCapture(); }
}

void ViewerWindow::on_keydown(WPARAM vk) {
    switch (vk) {
        case VK_OEM_PLUS: case VK_ADD:
            set_zoom(1.25f, 0, 0); update_title(); invalidate_viewport(); break;
        case VK_OEM_MINUS: case VK_SUBTRACT:
            set_zoom(1.0f / 1.25f, 0, 0); update_title(); invalidate_viewport(); break;
        case 'F':
            zoom_to_fit(); update_title(); invalidate_viewport(); break;
        case '1':
            zoom_ = 1.0f; offset_x_ = offset_y_ = 0.0f;
            update_title(); invalidate_viewport(); break;
        case 'A': {
            const bool show = !analysis_.visible();
            analysis_.set_visible(show);
            toolbar_.set_analysis_active(show);
            layout();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case 'H': {
            const bool show = !headers_.visible();
            headers_.set_visible(show);
            toolbar_.set_headers_active(show);
            layout();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case 'O':
            if (::GetKeyState(VK_CONTROL) & 0x8000) on_open_dialog();
            break;
    }
}

void ViewerWindow::on_command(int id) {
    switch (id) {
        case kCmd_Open:           on_open_dialog(); break;
        case kCmd_PrevFile:       navigate_sibling(-1); ::SetFocus(hwnd_); break;
        case kCmd_NextFile:       navigate_sibling(+1); ::SetFocus(hwnd_); break;
        case kCmd_ToggleAnalysis: {
            const bool show = !analysis_.visible();
            analysis_.set_visible(show);
            toolbar_.set_analysis_active(show);
            layout();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_ToggleHeaders: {
            const bool show = !headers_.visible();
            headers_.set_visible(show);
            toolbar_.set_headers_active(show);
            layout();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_RotateLeft: {
            rotation_deg_ = (rotation_deg_ + 270) % 360;
            // Rotation swaps the bbox dims, so the previous pan extent may
            // now exceed the new bounds (or shift to a different axis).
            clamp_offset();
            update_title();
            invalidate_viewport();
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_RotateRight: {
            rotation_deg_ = (rotation_deg_ + 90) % 360;
            clamp_offset();
            update_title();
            invalidate_viewport();
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_StretchNone: {
            stretch_mode_ = StretchMode::None;
            toolbar_.set_stretch_none_active(true);
            toolbar_.set_stretch_auto_active(false);
            apply_stretch();
            invalidate_viewport();
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_StretchAuto: {
            stretch_mode_ = StretchMode::Auto;
            toolbar_.set_stretch_auto_active(true);
            toolbar_.set_stretch_none_active(false);
            apply_stretch();
            invalidate_viewport();
            ::SetFocus(hwnd_);
            break;
        }
        case kCmd_FitToWindow:
            zoom_to_fit(); update_title(); invalidate_viewport(); break;
        case kCmd_Zoom100:
            zoom_ = 1.0f; offset_x_ = offset_y_ = 0.0f;
            update_title(); invalidate_viewport(); break;
        case kCmd_ZoomIn: {
            const RECT vp = viewport_rect();
            set_zoom(1.25f, (vp.left + vp.right) / 2, (vp.top + vp.bottom) / 2);
            update_title(); invalidate_viewport();
            break;
        }
        case kCmd_ZoomOut: {
            const RECT vp = viewport_rect();
            set_zoom(1.0f / 1.25f, (vp.left + vp.right) / 2, (vp.top + vp.bottom) / 2);
            update_title(); invalidate_viewport();
            break;
        }
    }
}

void ViewerWindow::on_drop(HDROP drop) {
    wchar_t path[MAX_PATH] = {};
    if (::DragQueryFileW(drop, 0, path, MAX_PATH)) {
        load_file(path);
    }
    ::DragFinish(drop);
}

void ViewerWindow::on_open_dialog() {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Astro images\0*.fit;*.fits;*.xisf\0FITS\0*.fit;*.fits\0XISF\0*.xisf\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (::GetOpenFileNameW(&ofn)) {
        load_file(buf);
    }
}
