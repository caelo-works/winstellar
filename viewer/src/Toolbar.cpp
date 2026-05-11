#include "Toolbar.h"

#include <uxtheme.h>
#include <commctrl.h>

namespace {

// These IDs MUST match the kCmd_* values in ViewerWindow.cpp — duplicated
// (rather than #included) to keep Toolbar self-contained.
constexpr int kCmd_Open           = 100;
constexpr int kCmd_ToggleHeaders  = 101;
constexpr int kCmd_FitToWindow    = 102;
constexpr int kCmd_Zoom100        = 103;
constexpr int kCmd_ZoomIn         = 104;
constexpr int kCmd_ZoomOut        = 105;
constexpr int kCmd_ToggleAnalysis = 106;
constexpr int kCmd_RotateLeft     = 107;
constexpr int kCmd_RotateRight    = 108;
constexpr int kCmd_StretchNone    = 109;
constexpr int kCmd_StretchAuto    = 110;

constexpr int kButtonW = 44;
constexpr int kButtonH = 44;  // matches Toolbar::kHeight so buttons fill the bar

// App theme colors (mirrors ViewerWindow's constants).
constexpr COLORREF kBg          = RGB(0x0b, 0x0d, 0x10);
constexpr COLORREF kBgHover     = RGB(0x18, 0x1c, 0x22);
constexpr COLORREF kBgPressed   = RGB(0x22, 0x28, 0x30);
constexpr COLORREF kFg          = RGB(0xE0, 0xE2, 0xE5);
constexpr COLORREF kAccent      = RGB(0x7d, 0xd3, 0xfc);
constexpr COLORREF kSeparator   = RGB(0x22, 0x26, 0x2c);

struct ButtonSpec {
    int            cmd;
    const wchar_t* glyph;     // Segoe MDL2 Assets char, OR "1:1" text. Empty = separator.
    const wchar_t* tooltip;   // shown in tooltip on hover
    bool           is_text;   // true for label fonts (e.g. "1:1"), false for MDL2 glyph
    bool           toggle;    // BTNS_CHECK, retains pressed state
};

LRESULT CALLBACK toolbar_subproc(HWND h, UINT m, WPARAM w, LPARAM l,
                                 UINT_PTR id, DWORD_PTR /*ref*/) {
    if (m == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(w);
        RECT rc;
        ::GetClientRect(h, &rc);
        HBRUSH br = ::CreateSolidBrush(kBg);
        ::FillRect(hdc, &rc, br);
        ::DeleteObject(br);
        return 1;
    }
    if (m == WM_NCDESTROY) {
        ::RemoveWindowSubclass(h, toolbar_subproc, id);
    }
    return ::DefSubclassProc(h, m, w, l);
}

// Layout: Open | sep | Fit / 1:1 / Z- / Z+ | sep | Rot L / Rot R | sep |
//         RAW / Auto (stretch) | sep | Analysis / Headers (sidebar toggles)
constexpr ButtonSpec kButtons[] = {
    { kCmd_Open,            L"\xE8E5", L"Open... (Ctrl+O)",                false, false },
    { 0,                    L"",       L"",                                  false, false }, // separator
    { kCmd_FitToWindow,     L"\xE740", L"Fit to window (F)",                false, false },
    { kCmd_Zoom100,         L"1:1",    L"Actual size (1)",                  true,  false },
    { kCmd_ZoomOut,         L"\xE71F", L"Zoom out (-)",                     false, false },
    { kCmd_ZoomIn,          L"\xE8A3", L"Zoom in (+)",                      false, false },
    { 0,                    L"",       L"",                                  false, false }, // separator
    { kCmd_RotateLeft,      L"↺", L"Rotate 90° left (Shift+R)",   true,  false },
    { kCmd_RotateRight,     L"↻", L"Rotate 90° right (R)",        true,  false },
    { 0,                    L"",       L"",                                  false, false }, // separator
    { kCmd_StretchNone,     L"RAW",    L"No stretch (linear, 0..max)",      true,  true },
    { kCmd_StretchAuto,     L"Auto",   L"Auto stretch (PixInsight AutoSTF)", true, true },
    { 0,                    L"",       L"",                                  false, false }, // separator
    { kCmd_ToggleAnalysis,  L"\xE9F9", L"Show / hide measurements (A)",     false, true },
    { kCmd_ToggleHeaders,   L"\xE946", L"Show / hide FITS headers (H)",     false, true },
};

}  // namespace

bool Toolbar::create(HWND parent, HINSTANCE hinst) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    ::InitCommonControlsEx(&icc);

    hwnd_ = ::CreateWindowExW(
        0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
            CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
        0, 0, 0, 0, parent, nullptr, hinst, nullptr);
    if (!hwnd_) return false;

    // 0 = no image list; we draw the glyph ourselves in NM_CUSTOMDRAW.
    ::SendMessageW(hwnd_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    ::SendMessageW(hwnd_, TB_SETBUTTONSIZE, 0, MAKELPARAM(kButtonW, kButtonH));
    ::SendMessageW(hwnd_, TB_SETPADDING, 0, MAKELPARAM(0, 0));
    ::SendMessageW(hwnd_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);

    // Fonts: glyphs from Segoe MDL2 Assets, text labels from Segoe UI Semibold.
    LOGFONTW lf{};
    lf.lfHeight     = -16;
    lf.lfWeight     = FW_NORMAL;
    lf.lfCharSet    = DEFAULT_CHARSET;
    lf.lfQuality    = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
    mdl2_font_ = ::CreateFontIndirectW(&lf);

    lf.lfHeight = -13;
    lf.lfWeight = FW_SEMIBOLD;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    label_font_ = ::CreateFontIndirectW(&lf);

    TBBUTTON tbb[ARRAYSIZE(kButtons)] = {};
    for (size_t i = 0; i < ARRAYSIZE(kButtons); ++i) {
        const auto& s = kButtons[i];
        tbb[i].iBitmap = I_IMAGENONE;
        if (s.cmd == 0) {
            tbb[i].fsStyle = BTNS_SEP;
            tbb[i].fsState = TBSTATE_ENABLED;
            tbb[i].idCommand = 0;
        } else {
            // Without BTNS_AUTOSIZE, TB_SETBUTTONSIZE is honored.
            tbb[i].fsStyle = BTNS_BUTTON;
            if (s.toggle) tbb[i].fsStyle |= BTNS_CHECK;
            tbb[i].fsState   = TBSTATE_ENABLED;
            tbb[i].idCommand = s.cmd;
            tbb[i].iString   = 0;
        }
    }
    ::SendMessageW(hwnd_, TB_ADDBUTTONS, ARRAYSIZE(kButtons),
                   reinterpret_cast<LPARAM>(tbb));

    // The auto-tooltip control needs to know we want UNICODE notifications.
    HWND tip = reinterpret_cast<HWND>(::SendMessageW(hwnd_, TB_GETTOOLTIPS, 0, 0));
    if (tip) {
        ::SendMessageW(tip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
        ::SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 320);
    }

    // Subclass to paint the toolbar's client bg ourselves — by default the
    // control erases to system 3D-face (light grey), which looks awful in
    // dark mode. Also makes ALL of the toolbar (including the area below the
    // buttons and the right-of-last-button blank space) match the theme.
    ::SetWindowSubclass(hwnd_, toolbar_subproc, /*id=*/1, /*ref=*/0);

    return true;
}

void Toolbar::destroy() {
    if (mdl2_font_)  { ::DeleteObject(mdl2_font_);  mdl2_font_  = nullptr; }
    if (label_font_) { ::DeleteObject(label_font_); label_font_ = nullptr; }
    if (hwnd_)       { ::DestroyWindow(hwnd_);      hwnd_       = nullptr; }
}

void Toolbar::resize(int x, int y, int cx) {
    if (hwnd_) ::MoveWindow(hwnd_, x, y, cx, kHeight, TRUE);
}

void Toolbar::set_headers_active(bool on) {
    if (!hwnd_) return;
    ::SendMessageW(hwnd_, TB_CHECKBUTTON, kCmd_ToggleHeaders, MAKELPARAM(on ? TRUE : FALSE, 0));
}

void Toolbar::set_analysis_active(bool on) {
    if (!hwnd_) return;
    ::SendMessageW(hwnd_, TB_CHECKBUTTON, kCmd_ToggleAnalysis, MAKELPARAM(on ? TRUE : FALSE, 0));
}

void Toolbar::set_stretch_auto_active(bool on) {
    if (!hwnd_) return;
    ::SendMessageW(hwnd_, TB_CHECKBUTTON, kCmd_StretchAuto, MAKELPARAM(on ? TRUE : FALSE, 0));
}

void Toolbar::set_stretch_none_active(bool on) {
    if (!hwnd_) return;
    ::SendMessageW(hwnd_, TB_CHECKBUTTON, kCmd_StretchNone, MAKELPARAM(on ? TRUE : FALSE, 0));
}

LRESULT Toolbar::on_customdraw(LPNMTBCUSTOMDRAW nm) const {
    switch (nm->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: {
            // Paint the ENTIRE toolbar client rect dark first — the standard
            // toolbar erases unused space (right of the last button) with the
            // 3D-face system color and our WM_ERASEBKGND subclass alone isn't
            // enough because the control re-paints in its WM_PAINT.
            HBRUSH br = ::CreateSolidBrush(kBg);
            ::FillRect(nm->nmcd.hdc, &nm->nmcd.rc, br);
            ::DeleteObject(br);
            return CDRF_NOTIFYITEMDRAW;
        }

        case CDDS_ITEMPREPAINT: {
            HDC  hdc = nm->nmcd.hdc;
            RECT rc  = nm->nmcd.rc;
            const DWORD st = nm->nmcd.uItemState;
            const int   id = static_cast<int>(nm->nmcd.dwItemSpec);

            // 1. Background -- always paint, including over any default chrome.
            COLORREF bg = kBg;
            if      (st & CDIS_SELECTED) bg = kBgPressed;
            else if (st & CDIS_HOT)      bg = kBgHover;
            HBRUSH br = ::CreateSolidBrush(bg);
            ::FillRect(hdc, &rc, br);
            ::DeleteObject(br);

            // 2. Find the spec for this command. Skip if unknown (shouldn't happen).
            const ButtonSpec* spec = nullptr;
            for (const auto& s : kButtons) {
                if (s.cmd == id) { spec = &s; break; }
            }
            if (!spec) return CDRF_SKIPDEFAULT;

            // 3. Glyph / label. Hovered or pressed → accent cyan, otherwise neutral light.
            HFONT font = spec->is_text ? label_font_ : mdl2_font_;
            HFONT old  = static_cast<HFONT>(::SelectObject(hdc, font));
            COLORREF fg = ((st & CDIS_HOT) || (st & CDIS_SELECTED) || (st & CDIS_CHECKED))
                          ? kAccent : kFg;
            ::SetTextColor(hdc, fg);
            ::SetBkMode(hdc, TRANSPARENT);
            ::DrawTextW(hdc, spec->glyph, -1, &rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject(hdc, old);

            // 4. Indicator strip under active toggle (headers visible).
            if (spec->toggle && (st & CDIS_CHECKED)) {
                RECT strip = rc;
                strip.top   = strip.bottom - 2;
                HBRUSH ab = ::CreateSolidBrush(kAccent);
                ::FillRect(hdc, &strip, ab);
                ::DeleteObject(ab);
            }

            return CDRF_SKIPDEFAULT;
        }
    }
    return CDRF_DODEFAULT;
}

void Toolbar::on_tooltip(LPNMTTDISPINFOW nm) const {
    const int id = static_cast<int>(nm->hdr.idFrom);
    for (const auto& s : kButtons) {
        if (s.cmd == id) {
            // The lpszText buffer in NMTTDISPINFOW is shared; pointing at our
            // const string is fine because the toolbar copies it.
            nm->lpszText = const_cast<LPWSTR>(s.tooltip);
            nm->hinst    = nullptr;
            return;
        }
    }
}
