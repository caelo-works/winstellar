#pragma once

#include <windows.h>
#include <shellapi.h>   // HDROP
#include <d2d1.h>

#include <string>

#include "fits_core/fits_image.h"
#include "fits_core/fits_render.h"
#include "fits_core/fits_stretch.h"

#include "HeaderView.h"
#include "AnalysisView.h"
#include "Toolbar.h"

class ViewerWindow {
public:
    bool create(HINSTANCE hinst, const wchar_t* initial_path);
    int run_message_loop();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void init_d2d_factory();
    void create_render_target();
    void release_render_target();
    bool ensure_d2d_bitmap();

    void load_file(const wchar_t* path);
    void render();
    void layout();
    RECT viewport_rect() const;       // image area, excludes headers panel
    void invalidate_viewport();       // repaint the image area only (no flicker on listview)
    void update_title();              // refresh main title with zoom%

    void on_size(int cx, int cy);
    void on_paint();
    void on_wheel(int delta, int x, int y);
    void on_lbutton_down(int x, int y);
    void on_mouse_move(int x, int y);
    void on_lbutton_up();
    void on_keydown(WPARAM vk);
    void on_drop(HDROP drop);
    void on_open_dialog();
    void on_command(int id);

    void zoom_to_fit();
    void set_zoom(float factor, int anchor_x, int anchor_y);
    void clamp_offset();
    float min_zoom() const;   // fit-to-window zoom (lower bound for the user)

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    HACCEL accel_ = nullptr;
    AnalysisView analysis_;
    HeaderView headers_;
    Toolbar toolbar_;

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1Bitmap* bitmap_ = nullptr;

    fitsx::FitsImage image_;
    fitsx::StretchParams stretch_;
    fitsx::RenderedBitmap rendered_;
    std::wstring loaded_path_;

    enum class StretchMode { Auto, None };
    StretchMode stretch_mode_ = StretchMode::Auto;
    int rotation_deg_ = 0;  // 0 / 90 / 180 / 270 — clockwise

    void apply_stretch();   // recompute params for current mode + re-render bitmap

    float zoom_ = 1.0f;
    float offset_x_ = 0.0f;
    float offset_y_ = 0.0f;

    bool dragging_ = false;
    POINT drag_start_ = {};
    float drag_start_off_x_ = 0.0f;
    float drag_start_off_y_ = 0.0f;
};
