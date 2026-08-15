#pragma once

#include <windows.h>

#include <string>

// "About WinStellar": logo, version and build, licence, links, and the update
// setting -- which until now existed only as a registry value, for a feature
// that makes a network call by default.
//
// Painted with GDI rather than Direct2D like the inspection popups: it is a
// static page with an icon and hyperlinks, and DrawIconEx renders the app's
// resource icon directly where D2D would need a WIC conversion for it.
class AboutWindow {
public:
    // Modal-ish: creates, shows and pumps until closed. Owner is disabled while
    // it is up, matching how a real About box behaves.
    void show(HWND owner, HINSTANCE hinst);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void on_paint(HWND hwnd);
    void on_click(HWND hwnd, int x, int y);
    bool on_mousemove(HWND hwnd, int x, int y);   // true when over a hotspot

    // Hit rectangles, recomputed on every paint so they always match what is
    // drawn -- a hard-coded table drifts the moment a line is added.
    enum class Hot { None, Website, Releases, Issues, Licence, UpdateToggle, CheckNow, Close };
    struct Zone { RECT rc; Hot what; };
    Zone zones_[8] = {};
    int  zone_count_ = 0;
    Hot  hover_ = Hot::None;

    HFONT title_font_ = nullptr;
    HFONT body_font_  = nullptr;
    HFONT small_font_ = nullptr;
    HICON icon_       = nullptr;
    HINSTANCE hinst_  = nullptr;
    std::wstring* status_ = nullptr;   // transient line under the buttons
};
