#pragma once

#include <windows.h>
#include <objidl.h>
#include <propsys.h>
#include <propkey.h>

#include <utility>
#include <vector>

#include "StreamBuffer.h"

class FitsPropertyHandler : public IInitializeWithStream,
                            public IPropertyStore,
                            public IPropertyStoreCapabilities {
public:
    static HRESULT CreateInstance(REFIID riid, void** ppv);

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) override;

    // IPropertyStore
    IFACEMETHODIMP GetCount(DWORD* count) override;
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override;
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override;
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT value) override;
    IFACEMETHODIMP Commit() override;

    // IPropertyStoreCapabilities
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key) override;

private:
    FitsPropertyHandler();
    ~FitsPropertyHandler();

    void clear_props();
    void populate();

    LONG ref_ = 1;
    StreamBuffer buf_;
    bool populated_ = false;
    std::vector<std::pair<PROPERTYKEY, PROPVARIANT>> props_;
};
