#pragma once

#include <windows.h>
#include <commctrl.h>

#include <vector>

namespace fitsx { struct FitsHeader; }

class HeaderView {
public:
    bool create(HWND parent, HINSTANCE hinst, int id);
    void update(const std::vector<fitsx::FitsHeader>& headers);
    void clear();
    void set_visible(bool visible);
    bool visible() const noexcept { return visible_; }
    void resize(int x, int y, int cx, int cy);
    HWND hwnd() const noexcept { return list_; }

    // Copies selected rows (or all rows if none selected) to clipboard as
    // tab-separated "key\tvalue\nkey\tvalue\n..." (CF_UNICODETEXT).
    void copy_selection_to_clipboard();

private:
    HWND list_ = nullptr;
    bool visible_ = true;
};
