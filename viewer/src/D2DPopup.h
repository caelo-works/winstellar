#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>

#include <algorithm>

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
