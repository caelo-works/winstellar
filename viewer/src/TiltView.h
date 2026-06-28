#pragma once

#include <windows.h>
#include <d2d1.h>

#include "fits_core/analysis.h"

// Modeless top-level popup showing the classic tilt diagram: a square with its
// two diagonals and the median HFR at the five nodes (four corners + centre).
// Owner = the main viewer window; closing it just hides it. Purely a view -- the
// viewer feeds it a TiltResult via set_tilt() whenever the detailed analysis
// (re)computes.
class TiltWindow {
public:
    bool create(HWND owner, HINSTANCE hinst);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool is_visible() const noexcept { return hwnd_ && ::IsWindowVisible(hwnd_); }
    void show();
    void hide();
    void toggle() { is_visible() ? hide() : show(); }

    void set_tilt(const fitsx::TiltResult& t);
    void set_rotation(int deg);   // mirror the main view's cw rotation
    void clear();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void init_d2d();
    void release_d2d();
    void render();
    void on_paint();
    void on_size(int cx, int cy);

    HWND      hwnd_  = nullptr;
    HINSTANCE hinst_ = nullptr;

    ID2D1Factory*             d2d_factory_   = nullptr;
    ID2D1HwndRenderTarget*    rt_            = nullptr;
    ID2D1SolidColorBrush*     brush_         = nullptr;   // colour reset per draw
    struct IDWriteFactory*    dwrite_factory_ = nullptr;
    struct IDWriteTextFormat* text_          = nullptr;   // node values
    struct IDWriteTextFormat* head_          = nullptr;   // header line

    fitsx::TiltResult tilt_{};
    int  rot_ = 0;                 // 0/90/180/270 cw
    bool have_tilt_ = false;
};
