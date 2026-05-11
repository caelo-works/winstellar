#include "AnalysisView.h"

#include "fits_core/analysis.h"

#include <cmath>
#include <cstdio>
#include <string>

bool AnalysisView::create(HWND parent, HINSTANCE hinst, int id) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
    ::InitCommonControlsEx(&icc);

    list_ = ::CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS
            | LVS_NOSORTHEADER,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        hinst, nullptr);
    if (!list_) return false;

    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.cx = 130;
    col.iSubItem = 0;
    col.pszText = const_cast<LPWSTR>(L"Metric");
    ListView_InsertColumn(list_, 0, &col);

    col.cx = 170;
    col.iSubItem = 1;
    col.pszText = const_cast<LPWSTR>(L"Value");
    ListView_InsertColumn(list_, 1, &col);
    return true;
}

namespace {

void add_row(HWND list, int row, const wchar_t* label, const wchar_t* value) {
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = const_cast<LPWSTR>(label);
    ListView_InsertItem(list, &it);
    ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(value));
}

std::wstring fmt_double(double v, int decimals, const wchar_t* suffix = L"") {
    if (!std::isfinite(v)) return L"-";
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%.*f%s", decimals, v, suffix);
    return buf;
}

std::wstring fmt_int_value(double v, const wchar_t* suffix = L"") {
    if (!std::isfinite(v)) return L"-";
    wchar_t buf[64] = {};
    swprintf_s(buf, L"%lld%s",
               static_cast<long long>(std::llround(v)), suffix);
    return buf;
}

}  // namespace

void AnalysisView::update(const fitsx::AnalysisResult& r) {
    if (!list_) return;
    ListView_DeleteAllItems(list_);
    if (!r.success) {
        add_row(list_, 0, L"Status", L"analysis failed");
        return;
    }

    int row = 0;
    if (r.star_count > 0) {
        wchar_t v[16] = {};
        swprintf_s(v, L"%d", r.star_count);
        add_row(list_, row++, L"Stars",        v);
        add_row(list_, row++, L"HFR (px)",     fmt_double(r.hfr_median, 2).c_str());
        add_row(list_, row++, L"HFR SD",       fmt_double(r.hfr_stddev, 2).c_str());
        add_row(list_, row++, L"FWHM (px)",    fmt_double(r.fwhm_median, 2).c_str());
        add_row(list_, row++, L"Eccentricity", fmt_double(r.eccentricity_median, 2).c_str());
    } else {
        add_row(list_, row++, L"Stars",        L"0 (no detection)");
    }

    add_row(list_, row++, L"Pixel mean",   fmt_int_value(r.mean).c_str());
    add_row(list_, row++, L"Pixel SD",     fmt_int_value(r.stddev).c_str());
    add_row(list_, row++, L"Pixel median", fmt_int_value(r.median).c_str());
    add_row(list_, row++, L"Pixel MAD",    fmt_int_value(r.mad).c_str());

    {
        wchar_t buf[64];
        swprintf_s(buf, L"%g (%llux)", r.min_value,
                   static_cast<unsigned long long>(r.min_count));
        add_row(list_, row++, L"Min", buf);
        swprintf_s(buf, L"%g (%llux)", r.max_value,
                   static_cast<unsigned long long>(r.max_count));
        add_row(list_, row++, L"Max", buf);
    }
}

void AnalysisView::clear() {
    if (list_) ListView_DeleteAllItems(list_);
}

void AnalysisView::set_visible(bool v) {
    visible_ = v;
    if (list_) ::ShowWindow(list_, v ? SW_SHOW : SW_HIDE);
}

void AnalysisView::resize(int x, int y, int cx, int cy) {
    if (list_) ::MoveWindow(list_, x, y, cx, cy, TRUE);
}
