#pragma once

#include <unknwn.h>

// Defined in dllmain.cpp; also declared in Guids.h. A ClassFactory keeps the
// DLL loaded for its whole lifetime so COM's CoFreeUnusedLibraries can't unmap
// the module while a client still holds the factory returned by
// DllGetClassObject (which would turn the next call through its vtable into a
// use-after-unload crash of explorer.exe).
void DllAddRef();
void DllRelease();

class ClassFactory : public IClassFactory {
public:
    using CreateFn = HRESULT (*)(REFIID riid, void** ppv);

    explicit ClassFactory(CreateFn fn) noexcept : create_(fn) { DllAddRef(); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override;
    IFACEMETHODIMP LockServer(BOOL lock) override;

private:
    ~ClassFactory() { DllRelease(); }
    CreateFn create_;
    LONG ref_ = 1;
};
