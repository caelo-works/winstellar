#include "ClassFactory.h"
#include "Guids.h"

#include <new>

IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) ClassFactory::AddRef() {
    return InterlockedIncrement(&ref_);
}

IFACEMETHODIMP_(ULONG) ClassFactory::Release() {
    const ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!create_) return E_UNEXPECTED;
    return create_(riid, ppv);
}

IFACEMETHODIMP ClassFactory::LockServer(BOOL lock) {
    if (lock) DllAddRef(); else DllRelease();
    return S_OK;
}
