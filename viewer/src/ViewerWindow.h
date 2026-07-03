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
#include "fits_core/analysis.h"

#include "HeaderView.h"
#include "AnalysisView.h"
#include "Toolbar.h"
#include "Histogram.h"
#include "AberrationView.h"
#include "TiltView.h"
#include "BackgroundView.h"

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

    // Walk siblings in the current file's directory (FITS / XISF / camera RAW,
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

    // Inspection tools. The toolbar's Inspect button pops this menu; toggling
    // any overlay lazily kicks a detailed (per-star) analysis on the worker.
    void show_inspect_menu();
    void ensure_detailed();           // request detailed analysis if not ready
    void refresh_tilt_window();       // push the current tilt result if open
    void push_aberration();           // push frame (+stars) to the inspector if open
    void push_background();           // compute + push the background map if open
    // On-image overlay (star markers) drawn in draw_overlays().
    bool any_overlay_active() const noexcept {
        return show_stars_;
    }
    // Anything that consumes the per-star analysis (star overlay + tilt popup).
    // The aberration inspector's measured mode runs its own PSF plate, so it is
    // independent of this.
    bool needs_detailed() const noexcept {
        return any_overlay_active() || tilt_window_.is_visible();
    }
    // Map an image-space pixel (px,py) to a viewport screen point, honoring the
    // current zoom/pan and 90/180/270 rotation. Used to place overlays exactly
    // on top of the rendered bitmap.
    D2D1_POINT_2F image_to_screen(float px, float py) const;
    void draw_overlays();             // called from render(), after DrawBitmap

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
    AberrationWindow aberration_;
    TiltWindow       tilt_window_;
    BackgroundWindow background_window_;

    // Inspection toggles + the detailed per-star analysis they consume.
    // detailed_ is recomputed on the worker the first time something needs it
    // for the current image, then reused; cleared on every new load. Star
    // markers are an on-image overlay; tilt is its own popup window.
    bool show_stars_ = false;
    std::shared_ptr<const fitsx::DetailedAnalysis> detailed_;
    // Which detailed_ the tilt result was last computed for. compute_tilt is
    // rotation-independent (TiltView permutes at paint), so refresh_tilt_window
    // recomputes only when this changes -- not on every rotation keypress.
    const fitsx::DetailedAnalysis* tilt_computed_for_ = nullptr;
    bool detail_pending_ = false;     // a detailed analysis is in flight
    const fitsx::FitsImage* bg_for_ = nullptr;   // image the background map was built for

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1Bitmap* bitmap_ = nullptr;

    // Cached brushes for the loading-spinner overlay. Created lazily on
    // first paint, released with the render target.
    ID2D1SolidColorBrush* veil_brush_   = nullptr;
    ID2D1SolidColorBrush* spinner_brush_= nullptr;

    // Inspection-overlay brush. RT-bound (its colour is reset per primitive),
    // lazily initialized in draw_overlays().
    ID2D1SolidColorBrush*  overlay_brush_ = nullptr;   // released with the RT

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

    // loading_ drives the spinner overlay + Prev/Next gating.
    bool          loading_         = false;
    std::wstring  loading_filename_;   // shown under the spinner

    // Unified async worker -- one persistent thread, one slot per work
    // type (load + render), latest-wins. Eliminates the per-load
    // std::thread(...).detach() pattern that risked leaking unprocessed
    // LoadResult on shutdown when the user closed mid-load.
    //
    // Load takes priority over render: a fresh navigation against a new
    // file always supersedes a pending re-stretch of the previous one.
    void worker_main();
    void request_load (const std::wstring& path);
    void request_render(const fitsx::StretchParams& p);
    void request_detail();

    struct RenderResult;
    void on_render_finished(std::uint64_t gen, RenderResult* r);

    struct DetailResult;
    void on_detail_finished(std::uint64_t gen, DetailResult* r);

    // Background map, computed off the UI thread (scans every pixel + fits an
    // ABE surface -- seconds on a big frame; used to run inline in
    // push_background and freeze the UI).
    void request_background();
    struct BgResult;
    void on_bg_finished(std::uint64_t gen, BgResult* r);

    std::thread             worker_thread_;
    std::mutex              worker_mtx_;
    std::condition_variable worker_cv_;
    bool                    worker_quit_ = false;

    // load slot
    bool                                       pending_load_       = false;
    std::wstring                               pending_load_path_;
    std::uint64_t                              pending_load_gen_   = 0;
    StretchMode                                pending_load_mode_  = StretchMode::Auto;

    // render slot
    bool                                       pending_render_         = false;
    std::shared_ptr<const fitsx::FitsImage>    pending_render_img_;
    fitsx::StretchParams                       pending_render_params_{};
    std::uint64_t                              pending_render_gen_     = 0;

    // detail slot (per-star inspection analysis)
    bool                                       pending_detail_         = false;
    std::shared_ptr<const fitsx::FitsImage>    pending_detail_img_;
    std::uint64_t                              pending_detail_gen_     = 0;

    // background slot (sky-background / illumination map)
    bool                                       pending_bg_             = false;
    std::shared_ptr<const fitsx::FitsImage>    pending_bg_img_;
    std::uint64_t                              pending_bg_gen_         = 0;

    // UI-visible atomic mirrors -- on_*_finished compares the result's gen
    // against the latest to drop stale results.
    std::atomic<std::uint64_t> load_gen_latest_  {0};
    std::atomic<std::uint64_t> render_gen_latest_{0};
    std::atomic<std::uint64_t> detail_gen_latest_{0};
    std::atomic<std::uint64_t> bg_gen_latest_    {0};
};
