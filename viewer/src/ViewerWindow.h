#pragma once

#include <windows.h>
#include <shellapi.h>   // HDROP
#include <d2d1.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "fits_core/fits_image.h"
#include "fits_core/fits_render.h"
#include "fits_core/fits_stretch.h"

#include "HeaderView.h"
#include "AnalysisView.h"
#include "Toolbar.h"
#include "Histogram.h"

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

    // Kick off an async load on a worker thread. UI stays responsive; the
    // result is posted back via WM_APP_LOAD_DONE and committed on the UI
    // thread. Stale results (e.g. user spammed Next) are discarded via a
    // generation token.
    void load_file(const wchar_t* path);
    struct LoadResult;
    void on_load_finished(std::uint64_t gen, LoadResult* r);

    // Walk siblings in the current file's directory (.fit/.fits/.xisf,
    // natural-sort order) and load the next/previous one. step = +1 or -1.
    // Wraps around. No-op if no file is currently loaded.
    void navigate_sibling(int step);
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
    HistogramWindow histogram_;

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1Bitmap* bitmap_ = nullptr;

    // shared_ptr so an in-flight render worker can keep using the previous
    // image safely when the user navigates to the next file mid-render.
    std::shared_ptr<const fitsx::FitsImage> image_;
    fitsx::StretchParams  stretch_;
    fitsx::RenderedBitmap rendered_;
    std::wstring loaded_path_;

    enum class StretchMode { Auto, None, Custom };
    StretchMode stretch_mode_ = StretchMode::Auto;
    int rotation_deg_ = 0;  // 0 / 90 / 180 / 270 — clockwise

    // Recompute params for the current mode + kick an async re-render. Reads
    // image_; safe no-op when nothing is loaded.
    void apply_stretch();
    // Apply caller-provided params (e.g. from histogram sliders) and async-render.
    void apply_custom_stretch(const fitsx::StretchParams& p);
    // Reflect current mode/params on the toolbar's RAW/Auto check states.
    void refresh_stretch_toolbar();

    float zoom_ = 1.0f;
    float offset_x_ = 0.0f;
    float offset_y_ = 0.0f;

    bool dragging_ = false;
    POINT drag_start_ = {};
    float drag_start_off_x_ = 0.0f;
    float drag_start_off_y_ = 0.0f;

    // Async-load state. load_gen_ bumps for every begin_load() so workers
    // can tell whether their result is still wanted. loading_ drives the
    // spinner overlay + Prev/Next gating.
    std::atomic<std::uint64_t> load_gen_{0};
    bool          loading_         = false;
    std::wstring  loading_filename_;   // shown under the spinner

    // Dedicated render worker — one thread, one item in flight, coalesces
    // bursts of slider drags into "render the latest params only".
    // Lifecycle: started in create(), stopped in run_message_loop() teardown.
    void render_worker_main();
    void request_render(const fitsx::StretchParams& p);

    struct RenderResult;
    void on_render_finished(std::uint64_t gen, RenderResult* r);

    std::thread             render_thread_;
    std::mutex              render_mtx_;
    std::condition_variable render_cv_;
    bool                    render_quit_   = false;
    bool                    render_pending_= false;
    std::shared_ptr<const fitsx::FitsImage> render_img_;
    fitsx::StretchParams    render_params_{};
    std::uint64_t           render_gen_pending_ = 0;   // bumps on each request
    std::atomic<std::uint64_t> render_gen_latest_{0};  // mirror visible to UI
};
