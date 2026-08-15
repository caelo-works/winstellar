#pragma once

#include <windows.h>

// Dark popup menus with icons.
//
// A Win32 popup menu is system-drawn and light, which looks like a bug in an
// otherwise dark application. Owner-drawing the items gets the colours and the
// icons; SetMenuInfo with a dark background brush covers the surround the items
// do not paint (margins, the gutter), which is what an items-only approach
// leaves stubbornly white.
//
// Icons are Segoe MDL2 Assets glyphs, the same source the toolbar uses, so the
// two can never drift apart -- and no bitmap conversion is needed.

struct DarkMenuItem {
    const wchar_t* glyph;   // MDL2 code point, or nullptr for none
    const wchar_t* text;    // nullptr marks a separator
};

namespace darkmenu {

inline constexpr COLORREF kBg        = RGB(0x1b, 0x20, 0x27);
inline constexpr COLORREF kBgHot     = RGB(0x2a, 0x33, 0x3d);
inline constexpr COLORREF kText      = RGB(0xe8, 0xea, 0xed);
inline constexpr COLORREF kTextDim   = RGB(0x6b, 0x73, 0x80);
inline constexpr COLORREF kAccent    = RGB(0x22, 0xd3, 0xee);
inline constexpr COLORREF kSeparator = RGB(0x2f, 0x37, 0x42);

inline constexpr int kItemHeight = 30;
inline constexpr int kSepHeight  = 9;
inline constexpr int kGutter     = 34;   // icon column
inline constexpr int kPadRight   = 28;

// Long-lived GDI objects, created once. Menus are transient; recreating fonts
// per popup would churn handles for no reason.
inline HFONT& text_font() {
    static HFONT f = nullptr;
    if (!f) {
        LOGFONTW lf{};
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        f = ::CreateFontIndirectW(&lf);
    }
    return f;
}

inline HFONT& glyph_font() {
    static HFONT f = nullptr;
    if (!f) {
        LOGFONTW lf{};
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe MDL2 Assets");
        f = ::CreateFontIndirectW(&lf);
    }
    return f;
}

inline HBRUSH& background_brush() {
    static HBRUSH b = nullptr;
    if (!b) b = ::CreateSolidBrush(kBg);
    return b;
}

// Paint the menu's own surround dark, so the strip the items do not cover stops
// being white.
inline void apply(HMENU menu) {
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = background_brush();
    ::SetMenuInfo(menu, &mi);
}

// `spec` must outlive the menu (a static table is the intended use).
// `checked` items get an accent bar down the left edge -- the same signal the
// toolbar uses for an active button -- plus a check glyph when they carry no
// icon of their own.
inline void append(HMENU menu, int id, const DarkMenuItem* spec,
                   bool enabled = true, bool checked = false) {
    UINT flags = MF_OWNERDRAW | (spec && spec->text ? 0u : MF_SEPARATOR);
    if (!enabled) flags |= MF_GRAYED;
    if (checked)  flags |= MF_CHECKED;
    ::AppendMenuW(menu, flags, static_cast<UINT_PTR>(id),
                  reinterpret_cast<LPCWSTR>(spec));
}

// The canonical MDL2 check mark, used when a checkable item has no icon.
inline constexpr const wchar_t* kCheckGlyph = L"\xE73E";

// Hook these two from the owning window's WndProc.
inline bool on_measure_item(MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_MENU) return false;
    const auto* spec = reinterpret_cast<const DarkMenuItem*>(mis->itemData);
    if (!spec || !spec->text) {
        mis->itemHeight = kSepHeight;
        mis->itemWidth = 0;
        return true;
    }
    HDC dc = ::GetDC(nullptr);
    HGDIOBJ old = ::SelectObject(dc, text_font());
    RECT r{};
    ::DrawTextW(dc, spec->text, -1, &r, DT_CALCRECT | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::ReleaseDC(nullptr, dc);
    mis->itemWidth = static_cast<UINT>(kGutter + (r.right - r.left) + kPadRight);
    mis->itemHeight = kItemHeight;
    return true;
}

inline bool on_draw_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_MENU) return false;
    const auto* spec = reinterpret_cast<const DarkMenuItem*>(dis->itemData);
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;

    if (!spec || !spec->text) {
        HBRUSH bg = ::CreateSolidBrush(kBg);
        ::FillRect(dc, &rc, bg);
        ::DeleteObject(bg);
        HPEN pen = ::CreatePen(PS_SOLID, 1, kSeparator);
        HGDIOBJ old = ::SelectObject(dc, pen);
        const int y = (rc.top + rc.bottom) / 2;
        ::MoveToEx(dc, rc.left + 10, y, nullptr);
        ::LineTo(dc, rc.right - 10, y);
        ::SelectObject(dc, old);
        ::DeleteObject(pen);
        return true;
    }

    const bool hot      = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool checked  = (dis->itemState & ODS_CHECKED) != 0;

    HBRUSH bg = ::CreateSolidBrush(hot && !disabled ? kBgHot : kBg);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);

    if (checked && !disabled) {
        // Same "this is active" signal as a checked toolbar button.
        RECT bar{ rc.left, rc.top + 4, rc.left + 3, rc.bottom - 4 };
        HBRUSH ab = ::CreateSolidBrush(kAccent);
        ::FillRect(dc, &bar, ab);
        ::DeleteObject(ab);
    }

    ::SetBkMode(dc, TRANSPARENT);
    const COLORREF fg = disabled ? kTextDim : (hot ? kAccent : kText);
    ::SetTextColor(dc, fg);

    // A checkable item with no icon of its own uses the icon column to answer
    // the only question that varies for a toggle: is it on?
    const wchar_t* glyph = (spec->glyph && *spec->glyph) ? spec->glyph
                         : (checked ? kCheckGlyph : nullptr);
    if (glyph) {
        if (checked && !disabled && !hot) ::SetTextColor(dc, kAccent);
        HGDIOBJ old = ::SelectObject(dc, glyph_font());
        RECT gr{ rc.left + 10, rc.top, rc.left + kGutter - 6, rc.bottom };
        ::DrawTextW(dc, glyph, -1, &gr,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::SetTextColor(dc, fg);
    }

    HGDIOBJ old = ::SelectObject(dc, text_font());
    RECT tr{ rc.left + kGutter, rc.top, rc.right - 8, rc.bottom };
    ::DrawTextW(dc, spec->text, -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, old);
    return true;
}

}  // namespace darkmenu
