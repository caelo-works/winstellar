#include "HeaderView.h"

#include "fits_core/fits_image.h"

#include <string>

bool HeaderView::create(HWND parent, HINSTANCE hinst, int id) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
    ::InitCommonControlsEx(&icc);

    list_ = ::CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        hinst, nullptr);
    if (!list_) return false;

    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 100;
    col.iSubItem = 0;
    col.pszText = const_cast<LPWSTR>(L"Keyword");
    ListView_InsertColumn(list_, 0, &col);

    col.cx = 200;
    col.iSubItem = 1;
    col.pszText = const_cast<LPWSTR>(L"Value");
    ListView_InsertColumn(list_, 1, &col);
    return true;
}

static std::wstring widen_ascii(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back(static_cast<wchar_t>(c));
    return w;
}

void HeaderView::update(const std::vector<fitsx::FitsHeader>& headers) {
    if (!list_) return;
    ListView_DeleteAllItems(list_);
    int row = 0;
    for (const auto& h : headers) {
        if (h.key.empty()) continue;
        const std::wstring wkey = widen_ascii(h.key);
        const std::wstring wval = widen_ascii(h.value);

        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.iSubItem = 0;
        it.pszText = const_cast<LPWSTR>(wkey.c_str());
        ListView_InsertItem(list_, &it);

        ListView_SetItemText(list_, row, 1, const_cast<LPWSTR>(wval.c_str()));
        ++row;
    }
}

void HeaderView::clear() {
    if (list_) ListView_DeleteAllItems(list_);
}

void HeaderView::set_visible(bool v) {
    visible_ = v;
    if (list_) ::ShowWindow(list_, v ? SW_SHOW : SW_HIDE);
}

void HeaderView::resize(int x, int y, int cx, int cy) {
    if (list_) ::MoveWindow(list_, x, y, cx, cy, TRUE);
}

void HeaderView::copy_selection_to_clipboard() {
    if (!list_) return;
    const int total = ListView_GetItemCount(list_);
    const int sel_count = ListView_GetSelectedCount(list_);

    std::wstring out;
    out.reserve(static_cast<size_t>(total) * 64);

    auto append_row = [&](int row) {
        wchar_t key[80];
        wchar_t val[256];
        ListView_GetItemText(list_, row, 0, key, ARRAYSIZE(key));
        ListView_GetItemText(list_, row, 1, val, ARRAYSIZE(val));
        out.append(key);
        out.push_back(L'\t');
        out.append(val);
        out.push_back(L'\r');
        out.push_back(L'\n');
    };

    if (sel_count > 0) {
        int row = -1;
        while ((row = ListView_GetNextItem(list_, row, LVNI_SELECTED)) != -1) {
            append_row(row);
        }
    } else {
        for (int row = 0; row < total; ++row) append_row(row);
    }
    if (out.empty()) return;

    if (!::OpenClipboard(list_)) return;
    ::EmptyClipboard();
    const size_t bytes = (out.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        if (auto* p = static_cast<wchar_t*>(::GlobalLock(h))) {
            memcpy(p, out.c_str(), bytes);
            ::GlobalUnlock(h);
            if (!::SetClipboardData(CF_UNICODETEXT, h)) {
                ::GlobalFree(h);
            }
        } else {
            ::GlobalFree(h);
        }
    }
    ::CloseClipboard();
}
