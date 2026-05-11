#pragma once

#include <windows.h>
#include <commctrl.h>

namespace fitsx { struct AnalysisResult; }

// Two-column listview that surfaces the same 11 computed metrics that the
// shell extension exposes as Explorer columns. Sits at the top of the
// viewer's right-hand panel; the FITS-keyword HeaderView sits below it.
class AnalysisView {
public:
    bool create(HWND parent, HINSTANCE hinst, int id);
    void update(const fitsx::AnalysisResult& r);
    void clear();
    void set_visible(bool v);
    bool visible() const noexcept { return visible_; }
    void resize(int x, int y, int cx, int cy);
    HWND hwnd() const noexcept { return list_; }

    // Sized to fit all 11 rows + header without scrolling.
    static constexpr int kPreferredHeight = 280;

private:
    HWND list_ = nullptr;
    bool visible_ = true;
};
