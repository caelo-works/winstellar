#pragma once

#include <windows.h>
#include <objidl.h>
#include <ocidl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

#include "StreamBuffer.h"

class FitsPreviewHandler : public IInitializeWithStream,
                           public IPreviewHandler,
                           public IPreviewHandlerVisuals,
                           public IObjectWithSite,
                           public IOleWindow {
public:
    static HRESULT CreateInstance(REFIID riid, void** ppv);

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) override;

    // IPreviewHandler
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override;
    IFACEMETHODIMP SetRect(const RECT* prc) override;
    IFACEMETHODIMP DoPreview() override;
    IFACEMETHODIMP Unload() override;
    IFACEMETHODIMP SetFocus() override;
    IFACEMETHODIMP QueryFocus(HWND* phwnd) override;
    IFACEMETHODIMP TranslateAccelerator(MSG* pmsg) override;

    // IPreviewHandlerVisuals
    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override;
    IFACEMETHODIMP SetFont(const LOGFONTW* plf) override;
    IFACEMETHODIMP SetTextColor(COLORREF color) override;

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) override;
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override;

    // IOleWindow (required by IPreviewHandler indirectly via the host)
    IFACEMETHODIMP GetWindow(HWND* phwnd) override;
    IFACEMETHODIMP ContextSensitiveHelp(BOOL fEnter) override;

private:
    FitsPreviewHandler();
    ~FitsPreviewHandler();

    void release_site();
    void destroy_child();
    void create_child_if_needed();
    void load_and_render();
    void invalidate();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static const wchar_t* kWndClass;

    LONG ref_ = 1;
    StreamBuffer buf_;
    IUnknown* site_ = nullptr;
    HWND parent_ = nullptr;
    HWND child_ = nullptr;
    RECT rect_ = {};
    COLORREF bg_ = RGB(0, 0, 0);     // always black behind the image
    COLORREF fg_ = RGB(220, 220, 220);

    HBITMAP bitmap_ = nullptr;  // 32-bit BGRA DIB section
    int bmp_w_ = 0;
    int bmp_h_ = 0;
    bool render_failed_ = false;
    std::wstring error_text_;
};
