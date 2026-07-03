#pragma once

#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "fits_core/fits_render.h"
#include "fits_core/fits_image.h"
#include "fits_core/psf.h"

// Modeless top-level popup: the "Aberration Inspector". A dark toolbar (same
// look as the main window) toggles the grid (3x3 / 5x5) and the mode:
//   * Visuel   -- 100 % crops the user eyeballs (from the rendered frame).
//   * Inspecté -- a measured PSF plate: per zone, the best stars are detected
//     and measured with adaptive weighted moments (on the LINEAR image), and a
//     2.5-sigma ellipse + centroid is drawn over each.
//
// Owner = the main viewer window. The viewer pushes the rendered frame
// (set_source, for Visuel) and the linear image (set_image, for Inspecté).
class AberrationWindow {
public:
    enum class Mode { Visual, Inspected };

    bool create(HWND owner, HINSTANCE hinst);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool is_visible() const noexcept { return hwnd_ && ::IsWindowVisible(hwnd_); }
    void show();
    void hide();
    void toggle() { is_visible() ? hide() : show(); }

    int  grid() const noexcept { return grid_; }
    Mode mode() const noexcept { return mode_; }

    void set_on_layout_changed(std::function<void()> cb) { on_layout_changed_ = std::move(cb); }

    // Mirror the main view's clockwise rotation (0/90/180/270). Cells are
    // permuted and crops / stamps / ellipses rotated so the inspector matches
    // what's on screen. Marks the plate dirty; the next set_image rebuilds it.
    void set_rotation(int deg);

    // Linear image (for Inspecté). The PSF plate is recomputed only when the
    // image (or grid / rotation) actually changes (not on every re-stretch).
    void set_image(std::shared_ptr<const fitsx::FitsImage> img);
    // Rendered frame (for Visuel).
    void set_source(const fitsx::RenderedBitmap& rb);
    void clear();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void init_d2d();
    void release_d2d();
    void release_tiles();
    void render();
    void render_visual();
    void render_inspected();
    void on_paint();
    void on_size(int cx, int cy);
    void set_grid(int g);
    void set_mode(Mode m);
    int  tile_px() const;
    void resize_to_grid();
    void create_toolbar();
    LRESULT on_bar_customdraw(LPNMTBCUSTOMDRAW nm) const;

    void build_visual(const fitsx::RenderedBitmap& rb);  // (re)extract Visuel crops
    void build_inspected();          // (re)compute the PSF plate (expensive)
    void rebuild_inspected_display(); // rebuild rotated display zones from plate_

    static constexpr int kBarH = 40;

    // Visuel: one 100 % crop per cell.
    struct Crop {
        int w = 0, h = 0;
        std::vector<uint8_t> bgra;
    };
    // Inspecté: a measured star ready for display (stamp already rotated to the
    // displayed orientation, centroid/PA adjusted to match).
    struct DispStar {
        std::vector<uint8_t> bgra;     // kPsfStamp x kPsfStamp, BGRA (rotated)
        ID2D1Bitmap*         tex = nullptr;
        float x0 = 0, y0 = 0;          // centroid in the rotated stamp (px)
        float pa = 0, siga = 0, sigb = 0;
    };
    struct DispZone {
        std::vector<DispStar> stars;
        float elong = 0, ecc = 0;
        fitsx::PsfAxis axis = fitsx::PsfAxis::None;
    };

    HWND      hwnd_  = nullptr;
    HINSTANCE hinst_ = nullptr;
    HWND      bar_   = nullptr;
    HFONT     bar_font_ = nullptr;
    int       grid_  = 3;
    int       rot_   = 0;               // 0/90/180/270 cw, mirrors the main view
    Mode      mode_  = Mode::Visual;

    ID2D1Factory*          d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_          = nullptr;
    ID2D1SolidColorBrush*  br_panel_    = nullptr;
    ID2D1SolidColorBrush*  br_frame_    = nullptr;
    ID2D1SolidColorBrush*  br_accent_   = nullptr;
    struct IDWriteFactory*    dwrite_factory_ = nullptr;
    struct IDWriteTextFormat* text_           = nullptr;

    // Visuel state.
    std::vector<Crop>         crops_;
    std::vector<ID2D1Bitmap*> tiles_;
    bool have_crops_ = false;

    // Inspecté state.
    std::shared_ptr<const fitsx::FitsImage> img_;
    const fitsx::FitsImage*   plate_for_ = nullptr;   // image the plate was built for
    int                       plate_grid_ = 0;
    fitsx::PsfPlate           plate_;                 // cached detection (rotation-independent)
    std::vector<DispZone>      dzones_;                // displayed-grid order
    bool have_plate_ = false;

    std::function<void()> on_layout_changed_;
};
