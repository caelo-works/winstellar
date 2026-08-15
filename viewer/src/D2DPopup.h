#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>

#include <algorithm>
#include <string>

// Shared boilerplate for the D2D inspection popups (Tilt / Aberration /
// Background). They each register a dark tool window, host a 96-DPI
// HwndRenderTarget, and COM-release the same way. Kept as free helpers rather
// than a base class so every window keeps its own WndProc / layout / brushes.

template <typename T>
inline void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Win11 dark title bar + matching caption / border / text colours. The magic
// numbers are the DWMWINDOWATTRIBUTE values (immersive dark mode / caption /
// border / text colour) -- kept literal to avoid pulling the full dwmapi enum.
inline void apply_dark_titlebar(HWND hwnd, COLORREF caption,
                                COLORREF text = RGB(0xE8, 0xEA, 0xED)) {
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd, 20, &dark,    sizeof(dark));       // dark mode
    ::DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));    // caption colour
    ::DwmSetWindowAttribute(hwnd, 34, &caption, sizeof(caption));    // border colour
    ::DwmSetWindowAttribute(hwnd, 36, &text,    sizeof(text));       // text colour
}

// Remember where the user put an inspection popup. Only the main window used to
// save its geometry, so on a multi-monitor setup every popup had to be dragged
// back into place at each launch. Stored per popup under the same registry key
// as the main window's placement.
inline void save_popup_placement(HWND hwnd, const wchar_t* name) {
    if (!hwnd || !name) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    if (r.right <= r.left || r.bottom <= r.top) return;

    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WinStellar", 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &k, nullptr)
        != ERROR_SUCCESS) return;
    const DWORD v[4] = { static_cast<DWORD>(r.left), static_cast<DWORD>(r.top),
                         static_cast<DWORD>(r.right - r.left),
                         static_cast<DWORD>(r.bottom - r.top) };
    std::wstring value = std::wstring(L"Popup") + name;
    ::RegSetValueExW(k, value.c_str(), 0, REG_BINARY,
                     reinterpret_cast<const BYTE*>(v), sizeof(v));
    ::RegCloseKey(k);
}

// Move a freshly created popup back where it was left, if that position is still
// on a visible monitor -- a saved position from a monitor that is now unplugged
// would otherwise put the window somewhere unreachable.
inline void restore_popup_placement(HWND hwnd, const wchar_t* name) {
    if (!hwnd || !name) return;
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinStellar", 0, KEY_READ, &k)
        != ERROR_SUCCESS) return;
    DWORD v[4] = {};
    DWORD cb = sizeof(v), type = 0;
    std::wstring value = std::wstring(L"Popup") + name;
    const LSTATUS rc = ::RegQueryValueExW(k, value.c_str(), nullptr, &type,
                                          reinterpret_cast<BYTE*>(v), &cb);
    ::RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_BINARY || cb != sizeof(v)) return;
    if (v[2] == 0 || v[3] == 0) return;

    RECT r{ static_cast<LONG>(v[0]), static_cast<LONG>(v[1]),
            static_cast<LONG>(v[0] + v[2]), static_cast<LONG>(v[1] + v[3]) };
    if (!::MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;   // monitor is gone
    ::SetWindowPos(hwnd, nullptr, r.left, r.top,
                   static_cast<int>(v[2]), static_cast<int>(v[3]),
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

// Create a 96-DPI HwndRenderTarget sized to the window's client area (forcing 96
// DPI keeps our own coordinates 1:1 with pixels -- the main view does the same
// to avoid DWM double-scaling). Returns nullptr if the factory/hwnd is missing.
inline ID2D1HwndRenderTarget* create_hwnd_rt(ID2D1Factory* factory, HWND hwnd) {
    if (!factory || !hwnd) return nullptr;
    RECT rc; ::GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hprops = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)));
    ID2D1HwndRenderTarget* rt = nullptr;
    factory->CreateHwndRenderTarget(props, hprops, &rt);
    return rt;
}
