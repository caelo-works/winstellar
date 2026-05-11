#pragma once

#include <unknwn.h>

class ClassFactory : public IClassFactory {
public:
    using CreateFn = HRESULT (*)(REFIID riid, void** ppv);

    explicit ClassFactory(CreateFn fn) noexcept : create_(fn) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IClassFactory
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override;
    IFACEMETHODIMP LockServer(BOOL lock) override;

private:
    ~ClassFactory() = default;
    CreateFn create_;
    LONG ref_ = 1;
};
