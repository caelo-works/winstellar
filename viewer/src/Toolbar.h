#pragma once

#include <windows.h>
#include <commctrl.h>

// Top-of-window toolbar with dark-themed, custom-drawn buttons. Sends
// WM_COMMAND to its parent (matches the kCmd_* IDs already wired in
// ViewerWindow's WM_COMMAND handler).
class Toolbar {
public:
    bool create(HWND parent, HINSTANCE hinst);
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    static constexpr int kHeight = 44;

    void resize(int x, int y, int cx);

    // Forward custom-draw + tooltip notifications coming up through the
    // parent's WM_NOTIFY (the toolbar is a child of ViewerWindow, so all
    // notifications go there).
    LRESULT on_customdraw(LPNMTBCUSTOMDRAW nm) const;
    void    on_tooltip   (LPNMTTDISPINFOW nm) const;

    // Reflect each sidebar list's visibility on its toggle button.
    void set_analysis_active(bool on);
    void set_headers_active (bool on);
    // Stretch-mode toggles are mutually exclusive (radio-style).
    void set_stretch_auto_active(bool on);
    void set_stretch_none_active(bool on);
    // Greys out Prev/Next when no file is loaded.
    void set_nav_enabled(bool enabled);

private:
    HWND  hwnd_       = nullptr;
    HFONT mdl2_font_  = nullptr;   // Segoe MDL2 Assets (icon glyphs)
    HFONT label_font_ = nullptr;   // Segoe UI Semibold (text labels e.g. "1:1")
};
