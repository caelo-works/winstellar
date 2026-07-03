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

#include <delayimp.h>
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

// Delay-load hook (#5). The vendored dependency DLLs (cfitsio / sqlite3 /
// pugixml / raw_r, listed via /DELAYLOAD) are resolved here ONLY from this
// module's own directory -- never from the process CWD or PATH -- so a DLL
// planted alongside a FITS/RAW file (or on PATH) can't be loaded into
// explorer.exe. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR extends the same protection to
// each dependency's transitive deps (z.dll, pthreadVC3.dll, lcms2-2.dll);
// LOAD_LIBRARY_SEARCH_SYSTEM32 still lets the CRT / system DLLs resolve.
static FARPROC WINAPI winstellar_dli_hook(unsigned event, PDelayLoadInfo info) {
    if (event != dliNotePreLoadLibrary || !info || !info->szDll) return nullptr;

    wchar_t path[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(g_hInst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    slash[1] = L'\0';   // keep the trailing backslash, drop the file name

    wchar_t wdll[MAX_PATH];
    if (::MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, wdll, MAX_PATH) == 0)
        return nullptr;
    if (wcslen(path) + wcslen(wdll) >= MAX_PATH) return nullptr;
    wcscat_s(path, MAX_PATH, wdll);

    // Absolute path + these flags => loaded strictly from our directory / System32.
    HMODULE h = ::LoadLibraryExW(
        path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    return reinterpret_cast<FARPROC>(h);
}

// The delay-load helper picks this up by name (declared in delayimp.h).
extern "C" const PfnDliHook __pfnDliNotifyHook2 = winstellar_dli_hook;

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
