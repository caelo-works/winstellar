#pragma once

#include <windows.h>
#include <d2d1.h>

#include <cstdint>
#include <vector>

#include "fits_core/background.h"

// Modeless popup: the sky-background / illumination map. Shows a magma heatmap
// of the measured low-res illumination surface, a gradient arrow, and a metrics
// line (radial drop, gradient, azimuthal anisotropy, amp-glow). Resizable.
// Owner = the main viewer; the viewer feeds it a BackgroundMap via set_map().
class BackgroundWindow {
public:
    bool create(HWND owner, HINSTANCE hinst);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool is_visible() const noexcept { return hwnd_ && ::IsWindowVisible(hwnd_); }
    void show();
    void hide();
    void toggle() { is_visible() ? hide() : show(); }

    void set_map(const fitsx::BackgroundMap& m);
    void set_rotation(int deg);   // mirror the main view's cw rotation
    void clear();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void init_d2d();
    void release_d2d();
    void render();
    void on_paint();
    void on_size(int cx, int cy);
    void rebuild_heatmap();

    HWND      hwnd_  = nullptr;
    HINSTANCE hinst_ = nullptr;

    ID2D1Factory*             d2d_factory_   = nullptr;
    ID2D1HwndRenderTarget*    rt_            = nullptr;
    ID2D1Bitmap*              heat_          = nullptr;   // grid-res heatmap
    ID2D1SolidColorBrush*     brush_         = nullptr;
    struct IDWriteFactory*    dwrite_factory_ = nullptr;
    struct IDWriteTextFormat* text_          = nullptr;

    fitsx::BackgroundMap   map_;
    std::vector<uint8_t>   heat_bgra_;       // heat_w_*heat_h_*4 (already rotated)
    int  heat_w_ = 0, heat_h_ = 0;
    int  rot_ = 0;                           // 0/90/180/270 cw
    bool have_map_ = false;
};
