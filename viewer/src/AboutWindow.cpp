#include "AboutWindow.h"

#include "D2DPopup.h"      // apply_dark_titlebar
#include "UpdateCheck.h"
#include "fits_version.h"

#include <shellapi.h>
#include <windowsx.h>

#include <string>

namespace {

constexpr const wchar_t* kClassName = L"WinStellarAboutWindow";
constexpr int kWidth  = 460;
constexpr int kHeight = 690;

constexpr COLORREF kBg     = RGB(0x0f, 0x14, 0x1a);
constexpr COLORREF kText   = RGB(0xe8, 0xea, 0xed);
constexpr COLORREF kDim    = RGB(0x8b, 0x93, 0xa0);
constexpr COLORREF kAccent = RGB(0x22, 0xd3, 0xee);

constexpr const wchar_t* kWebsite  = L"https://winstellar.fr";
constexpr const wchar_t* kReleases = L"https://github.com/caelo-works/winstellar/releases";
constexpr const wchar_t* kIssues   = L"https://github.com/caelo-works/winstellar/issues";
constexpr const wchar_t* kLicence  = L"https://www.gnu.org/licenses/gpl-3.0.html";

HFONT make_font(int height, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return ::CreateFontIndirectW(&lf);
}

std::wstring widen(const char* s) {
    if (!s) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

void open_url(const wchar_t* url) {
    ::ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

LRESULT CALLBACK AboutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<AboutWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_PAINT:      self->on_paint(hwnd); return 0;
        case WM_ERASEBKGND: return 1;              // painted whole in WM_PAINT
        case WM_LBUTTONUP:
            self->on_click(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            if (self->on_mousemove(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return 0;
        case WM_SETCURSOR: {
            POINT pt{};
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            if (self->on_mousemove(hwnd, pt.x, pt.y)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { ::DestroyWindow(hwnd); return 0; }
            break;
        case WM_CLOSE:   ::DestroyWindow(hwnd); return 0;
        case WM_DESTROY: ::PostQuitMessage(0);   return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void AboutWindow::on_paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(hwnd, &ps);
    RECT rc{};
    ::GetClientRect(hwnd, &rc);

    // Double-buffer: this repaints on every hover change.
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

    HBRUSH bg = ::CreateSolidBrush(kBg);
    ::FillRect(mem, &rc, bg);
    ::DeleteObject(bg);
    ::SetBkMode(mem, TRANSPARENT);

    zone_count_ = 0;
    auto add_zone = [&](RECT r, Hot what) {
        if (zone_count_ < 8) zones_[zone_count_++] = { r, what };
    };
    auto line = [&](const wchar_t* text, int y, HFONT font, COLORREF colour,
                    UINT align = DT_CENTER) -> RECT {
        HGDIOBJ old = ::SelectObject(mem, font);
        ::SetTextColor(mem, colour);
        RECT r{ 20, y, rc.right - 20, y + 40 };
        ::DrawTextW(mem, text, -1, &r, align | DT_TOP | DT_SINGLELINE | DT_CALCRECT);
        // Centre the measured rect within the window.
        if (align == DT_CENTER) {
            const int w = r.right - r.left;
            r.left = (rc.right - w) / 2;
            r.right = r.left + w;
        }
        ::DrawTextW(mem, text, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
        ::SelectObject(mem, old);
        return r;
    };

    int y = 24;
    if (icon_) {
        ::DrawIconEx(mem, (rc.right - 64) / 2, y, icon_, 64, 64, 0, nullptr, DI_NORMAL);
        y += 78;
    }

    line(L"WinStellar", y, title_font_, kText);
    y += 34;

    const std::wstring version_line = L"Version " + widen(FITS_VERSION_STR);
    line(version_line.c_str(), y, body_font_, kText);
    y += 24;

    // The build string is what a bug report needs; showing it here saves the
    // support round-trip through Properties -> Details -> Comments.
    const std::wstring build_line = widen(FITS_VERSION_FULL_STR);
    line(build_line.c_str(), y, small_font_, kDim);
    y += 28;

    line(L"A fast FITS, XISF and camera-RAW viewer for Windows Explorer.",
         y, small_font_, kDim);
    y += 32;

    add_zone(line(L"GPL-3.0 — free and open source", y, small_font_,
                  hover_ == Hot::Licence ? kAccent : kDim), Hot::Licence);
    y += 34;

    add_zone(line(kWebsite, y, body_font_,
                  hover_ == Hot::Website ? kAccent : kText), Hot::Website);
    y += 26;
    add_zone(line(L"Releases and changelog", y, body_font_,
                  hover_ == Hot::Releases ? kAccent : kText), Hot::Releases);
    y += 26;
    add_zone(line(L"Report a problem", y, body_font_,
                  hover_ == Hot::Issues ? kAccent : kText), Hot::Issues);
    y += 34;

    const bool on = wsu::update_check_enabled();
    std::wstring toggle = on ? L"☑  Check for updates automatically"
                             : L"☐  Check for updates automatically";
    add_zone(line(toggle.c_str(), y, body_font_,
                  hover_ == Hot::UpdateToggle ? kAccent : kText), Hot::UpdateToggle);
    y += 26;

    add_zone(line(L"Check for updates now", y, body_font_,
                  hover_ == Hot::CheckNow ? kAccent : kDim), Hot::CheckNow);
    y += 24;

    if (status_ && !status_->empty()) {
        line(status_->c_str(), y, small_font_, kDim);
    }
    y += 30;

    // Keyboard shortcuts live here rather than in a second window: they are
    // reference material, and the support KB has always documented them as a
    // complete list the application itself never showed anyone.
    {
        HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(0x25, 0x2c, 0x35));
        HGDIOBJ old_pen = ::SelectObject(mem, pen);
        ::MoveToEx(mem, 40, y, nullptr);
        ::LineTo(mem, rc.right - 40, y);
        ::SelectObject(mem, old_pen);
        ::DeleteObject(pen);
        y += 14;

        line(L"Keyboard shortcuts", y, body_font_, kText);
        y += 26;

        struct Row { const wchar_t* key; const wchar_t* what; };
        static const Row rows[] = {
            { L"Ctrl+O",        L"Open a file" },
            { L"Ctrl+S",        L"Export the image" },
            { L"\u2190 / \u2192  ·  PgUp / PgDn", L"Previous / next frame in the folder" },
            { L"F",             L"Fit to window" },
            { L"1",             L"Actual size (100 %)" },
            { L"+ / \u2212",        L"Zoom in / out (the wheel zooms too)" },
            { L"R  ·  Shift+R", L"Rotate 90\u00b0 right / left" },
            { L"A",             L"Measurements panel" },
            { L"H",             L"FITS headers panel" },
            { L"Ctrl+H",        L"Histogram and stretch sliders" },
        };
        HGDIOBJ old = ::SelectObject(mem, small_font_);
        for (const Row& r : rows) {
            RECT kr{ 44, y, 210, y + 20 };
            ::SetTextColor(mem, kText);
            ::DrawTextW(mem, r.key, -1, &kr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            RECT vr{ 224, y, rc.right - 30, y + 20 };
            ::SetTextColor(mem, kDim);
            ::DrawTextW(mem, r.what, -1, &vr, DT_LEFT | DT_TOP | DT_SINGLELINE);
            y += 20;
        }
        ::SelectObject(mem, old);
        y += 6;
        ::SetTextColor(mem, kDim);
        HGDIOBJ old2 = ::SelectObject(mem, small_font_);
        RECT dr{ 44, y, rc.right - 30, y + 20 };
        ::DrawTextW(mem, L"Drag with the left mouse button to pan.", -1, &dr,
                    DT_LEFT | DT_TOP | DT_SINGLELINE);
        ::SelectObject(mem, old2);
    }

    ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, old_bmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd, &ps);
}

bool AboutWindow::on_mousemove(HWND hwnd, int x, int y) {
    const POINT pt{ x, y };
    Hot found = Hot::None;
    for (int i = 0; i < zone_count_; ++i)
        if (::PtInRect(&zones_[i].rc, pt)) { found = zones_[i].what; break; }
    if (found != hover_) {
        hover_ = found;
        ::InvalidateRect(hwnd, nullptr, FALSE);
    }
    return found != Hot::None;
}

void AboutWindow::on_click(HWND hwnd, int x, int y) {
    const POINT pt{ x, y };
    Hot what = Hot::None;
    for (int i = 0; i < zone_count_; ++i)
        if (::PtInRect(&zones_[i].rc, pt)) { what = zones_[i].what; break; }

    switch (what) {
        case Hot::Website:  open_url(kWebsite);  break;
        case Hot::Releases: open_url(kReleases); break;
        case Hot::Issues:   open_url(kIssues);   break;
        case Hot::Licence:  open_url(kLicence);  break;
        case Hot::UpdateToggle:
            wsu::set_update_check_enabled(!wsu::update_check_enabled());
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case Hot::CheckNow: {
            if (status_) *status_ = L"Checking…";
            ::InvalidateRect(hwnd, nullptr, FALSE);
            ::UpdateWindow(hwnd);
            const wsu::UpdateCheckResult r =
                wsu::check_for_update(wsu::parse_version(FITS_VERSION_STR));
            if (status_) {
                if (!r.error.empty())      *status_ = L"Could not reach the update server.";
                else if (r.available)      *status_ = L"A newer version is available — "
                                                      L"see Releases above.";
                else                       *status_ = L"You are running the latest version.";
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        default: break;
    }
}

void AboutWindow::show(HWND owner, HINSTANCE hinst) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(hinst, MAKEINTRESOURCEW(1));
    ::RegisterClassExW(&wc);

    title_font_ = make_font(26, FW_SEMIBOLD);
    body_font_  = make_font(15, FW_NORMAL);
    small_font_ = make_font(13, FW_NORMAL);
    icon_       = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                  64, 64, LR_DEFAULTCOLOR));
    std::wstring status;
    status_ = &status;

    RECT orc{};
    if (owner) ::GetWindowRect(owner, &orc);
    const int x = owner ? orc.left + ((orc.right - orc.left) - kWidth) / 2 : CW_USEDEFAULT;
    const int y = owner ? orc.top + ((orc.bottom - orc.top) - kHeight) / 3 : CW_USEDEFAULT;

    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"About WinStellar",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                  x, y, kWidth, kHeight, owner, nullptr, hinst, this);
    if (!hwnd) { status_ = nullptr; return; }
    apply_dark_titlebar(hwnd, kBg);
    ::ShowWindow(hwnd, SW_SHOW);
    if (owner) ::EnableWindow(owner, FALSE);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(hwnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (owner) { ::EnableWindow(owner, TRUE); ::SetActiveWindow(owner); }
    status_ = nullptr;
    if (icon_)       { ::DestroyIcon(icon_);      icon_ = nullptr; }
    if (title_font_) { ::DeleteObject(title_font_); title_font_ = nullptr; }
    if (body_font_)  { ::DeleteObject(body_font_);  body_font_  = nullptr; }
    if (small_font_) { ::DeleteObject(small_font_); small_font_ = nullptr; }
}
