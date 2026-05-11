#include <initguid.h>  // emit DEFINE_GUID/DEFINE_PROPERTYKEY definitions in this TU only
#include <windows.h>
#include <objidl.h>    // full COM/OLE types so propkeydef.h's C++ operators parse
#include <propsys.h>
#include <propkey.h>   // pulled in WITH initguid.h so standard PKEYs are emitted here

#include "Guids.h"
#include "ClassFactory.h"
#include "PreviewHandler.h"
#include "PropertyHandler.h"
#include "Registration.h"
#include "ThumbnailProvider.h"

#include <new>

HMODULE g_hInst = nullptr;
static LONG g_lock_count = 0;

void DllAddRef() { ::InterlockedIncrement(&g_lock_count); }
void DllRelease() { ::InterlockedDecrement(&g_lock_count); }

extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinst;
        ::DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

extern "C" HRESULT WINAPI
DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    ClassFactory::CreateFn fn = nullptr;
    if (IsEqualCLSID(rclsid, CLSID_FitsThumbnailProvider)) {
        fn = &FitsThumbnailProvider::CreateInstance;
    } else if (IsEqualCLSID(rclsid, CLSID_FitsPreviewHandler)) {
        fn = &FitsPreviewHandler::CreateInstance;
    } else if (IsEqualCLSID(rclsid, CLSID_FitsPropertyHandler)) {
        fn = &FitsPropertyHandler::CreateInstance;
    } else {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* cf = new (std::nothrow) ClassFactory(fn);
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return (g_lock_count == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    return RegisterShellExtensions();
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    return UnregisterShellExtensions();
}
