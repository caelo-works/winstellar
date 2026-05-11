#pragma once

#include <windows.h>
#include <d2d1.h>

#include <functional>
#include <memory>
#include <vector>

#include "fits_core/fits_image.h"
#include "fits_core/fits_stretch.h"

// Modeless top-level popup that lets the user shape the stretch curve by
// dragging shadow / midtone / highlight handles on a log-scaled histogram.
// Owner = the main viewer window; closing this popup hides it.
//
// The popup is purely a UI: it never mutates the underlying image. When the
// user adjusts a handle, it fires on_params_changed with the new
// StretchParams; the owner is responsible for re-rendering.
class HistogramWindow {
public:
    using ChangedCallback = std::function<void(const fitsx::StretchParams&)>;
    using AutoCallback    = std::function<void()>;   // user clicked "Auto"
    using RawCallback     = std::function<void()>;   // user clicked "RAW"

    bool create(HWND owner, HINSTANCE hinst);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool is_visible() const noexcept { return hwnd_ && ::IsWindowVisible(hwnd_); }
    void show();
    void hide();
    void toggle() { is_visible() ? hide() : show(); }

    // Push a new image and current params. The 256-bin histogram is
    // computed lazily on first show() (or on the next show after the image
    // changed) so file-load latency isn't burdened by ~300 ms of bin work
    // when the popup is hidden. Pass image=nullptr to clear.
    void set_image (std::shared_ptr<const fitsx::FitsImage> image);
    void set_params(const fitsx::StretchParams& p);

    void set_on_changed(ChangedCallback cb) { on_changed_ = std::move(cb); }
    void set_on_auto   (AutoCallback    cb) { on_auto_    = std::move(cb); }
    void set_on_raw    (RawCallback     cb) { on_raw_     = std::move(cb); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void init_d2d();
    void release_d2d();
    void render();
    void recompute_bins(const fitsx::FitsImage& img);

    // Geometry helpers — all in client-area DIPs.
    D2D1_RECT_F histogram_rect() const;
    D2D1_RECT_F slider_rect()    const;   // strip under the histogram with the 3 handles
    int   hit_test_handle(float x, float y) const;   // -1, 0=shadow, 1=mid, 2=high
    float x_to_norm(float x) const;       // viewport x -> [0,1]
    float norm_to_x(float n) const;       // [0,1] -> viewport x

    void on_mouse_down(int x, int y);
    void on_mouse_move(int x, int y);
    void on_mouse_up  ();
    void on_paint     ();
    void on_size      (int cx, int cy);
    void on_command   (int id);

    void apply_handles_to_params();       // recompute params_ from handle positions
    void apply_params_to_handles();       // inverse

    void on_drawitem(DRAWITEMSTRUCT* di);

    HWND      hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;

    ID2D1Factory*           d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget*  rt_          = nullptr;

    // Brushes + geometry cached on the render target. All are recreated on
    // D2DERR_RECREATE_TARGET via release_d2d() -> init_d2d().
    struct ID2D1SolidColorBrush* br_panel_  = nullptr;
    struct ID2D1SolidColorBrush* br_bar_    = nullptr;
    struct ID2D1SolidColorBrush* br_guide_  = nullptr;
    struct ID2D1SolidColorBrush* br_clip_   = nullptr;
    struct ID2D1SolidColorBrush* br_strip_  = nullptr;
    struct ID2D1SolidColorBrush* br_handle_ = nullptr;
    struct ID2D1PathGeometry*    diamond_   = nullptr;   // unit diamond at origin

    // 256-bin log-scaled counts. Computed lazily on first show() to keep
    // file loads fast: ~300 ms on 36 Mpx images, which is wasted work
    // whenever the popup is hidden (the common case).
    std::vector<float>                       bins_;
    float                                    bins_log_max_ = 0.0f;
    std::shared_ptr<const fitsx::FitsImage>  bins_pending_;   // non-null = dirty

    // Handle positions in normalized [0,1] data-range space.
    // shadows_ <= midtone_ <= highlights_  (enforced by hit-test/drag clamping)
    float shadows_    = 0.0f;
    float midtone_    = 0.5f;
    float highlights_ = 1.0f;

    fitsx::StretchParams params_{};   // last params_ derived from the handles

    int   drag_handle_ = -1;   // -1 = no drag, else 0/1/2

    HWND  btn_auto_ = nullptr;
    HWND  btn_raw_  = nullptr;
    HFONT btn_font_ = nullptr;

    ChangedCallback on_changed_;
    AutoCallback    on_auto_;
    RawCallback     on_raw_;
};
