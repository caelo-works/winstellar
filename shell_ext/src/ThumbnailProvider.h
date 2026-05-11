#pragma once

#include <windows.h>
#include <objidl.h>
#include <propsys.h>
#include <thumbcache.h>

#include "StreamBuffer.h"

class FitsThumbnailProvider : public IInitializeWithStream,
                              public IThumbnailProvider {
public:
    static HRESULT CreateInstance(REFIID riid, void** ppv);

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) override;

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    FitsThumbnailProvider();
    ~FitsThumbnailProvider();

    LONG ref_ = 1;
    StreamBuffer buf_;
};
