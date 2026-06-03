#include "Registration.h"
#include "Guids.h"

#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>

namespace {

constexpr const wchar_t* kProgId = L"WinStellar.Image";

// Extensions whose Explorer thumbnail / preview / property handlers we own.
// FITS + XISF plus the TIFF-based camera RAW set (mirrors raw_loader::is_raw
// and the viewer's has_astro_extension). Single source for both register and
// unregister so they can't drift.
constexpr const wchar_t* kAllExts[] = {
    L".fit", L".fits", L".xisf",
    L".nef", L".nrw", L".cr2", L".arw", L".sr2", L".dng", L".pef", L".srw", L".iiq",
};
// Subset we also claim as the default double-click association -- only the
// formats Windows has no native handler for. RAW keeps the user's photo app as
// its default opener; we only add our shell handlers + an "Open with" entry.
constexpr const wchar_t* kDefaultAssocExts[] = { L".fit", L".fits", L".xisf" };

constexpr const wchar_t* kClsIdRoot = L"Software\\Classes\\CLSID";

// Standard ShellEx subkey GUIDs (literal strings, well-known)
constexpr const wchar_t* kShellExPreview =
    L"ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
constexpr const wchar_t* kShellExThumbnail =
    L"ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";

constexpr const wchar_t* kPreviewHandlersList =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
constexpr const wchar_t* kPropHandlersBase =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers";

std::wstring guid_to_string(REFGUID g) {
    wchar_t buf[64] = {};
    ::StringFromGUID2(g, buf, ARRAYSIZE(buf));
    return std::wstring(buf);
}

std::wstring module_path() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(g_hInst, buf, ARRAYSIZE(buf));
    return std::wstring(buf);
}

// Path to WinStellar.exe staged next to the DLL by register.cmd.
std::wstring viewer_exe_path() {
    std::wstring p = module_path();
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return p.substr(0, slash + 1) + L"WinStellar.exe";
}

// Open or create a (possibly multi-level) subkey, creating every missing
// intermediate key. Caller must close the returned handle.
LSTATUS create_key(HKEY root, const std::wstring& sub, HKEY* out) {
    return ::RegCreateKeyExW(root, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_READ | KEY_WRITE, nullptr, out, nullptr);
}

LSTATUS write_string(HKEY root, const std::wstring& sub, const wchar_t* name,
                     const std::wstring& value) {
    HKEY h = nullptr;
    LSTATUS s = create_key(root, sub, &h);
    if (s != ERROR_SUCCESS) return s;
    s = ::RegSetValueExW(h, name, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(h);
    return s;
}

LSTATUS write_dword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD value) {
    HKEY h = nullptr;
    LSTATUS s = create_key(root, sub, &h);
    if (s != ERROR_SUCCESS) return s;
    s = ::RegSetValueExW(h, name, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&value), sizeof(value));
    ::RegCloseKey(h);
    return s;
}

LSTATUS delete_tree(HKEY root, const std::wstring& sub) {
    HKEY h = nullptr;
    if (::RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) {
        return ERROR_FILE_NOT_FOUND;
    }
    LSTATUS s = ::RegDeleteTreeW(h, nullptr);
    ::RegCloseKey(h);
    if (s == ERROR_SUCCESS) {
        // Delete the (now-empty) leaf key itself
        ::RegDeleteKeyW(root, sub.c_str());
    }
    return s;
}

LSTATUS register_clsid(HKEY root, REFCLSID clsid, const wchar_t* friendly,
                       const wchar_t* threading_model, REFGUID app_id /*may be GUID_NULL*/) {
    const std::wstring g = guid_to_string(clsid);
    const std::wstring base = std::wstring(kClsIdRoot) + L"\\" + g;
    const std::wstring inproc = base + L"\\InProcServer32";

    LSTATUS s;
    if ((s = write_string(root, base, nullptr, friendly)) != ERROR_SUCCESS) return s;
    if ((s = write_string(root, inproc, nullptr, module_path())) != ERROR_SUCCESS) return s;
    if ((s = write_string(root, inproc, L"ThreadingModel", threading_model)) != ERROR_SUCCESS) return s;

    if (!IsEqualGUID(app_id, GUID_NULL)) {
        if ((s = write_string(root, base, L"AppID", guid_to_string(app_id))) != ERROR_SUCCESS) return s;
    }
    return ERROR_SUCCESS;
}

LSTATUS register_extension(HKEY root, const wchar_t* ext,
                           REFCLSID preview, REFCLSID thumbnail) {
    const std::wstring base = std::wstring(L"Software\\Classes\\") + ext;

    LSTATUS s;
    {
        const std::wstring k = base + L"\\" + kShellExPreview;
        if ((s = write_string(root, k, nullptr, guid_to_string(preview))) != ERROR_SUCCESS) return s;
    }
    {
        const std::wstring k = base + L"\\" + kShellExThumbnail;
        if ((s = write_string(root, k, nullptr, guid_to_string(thumbnail))) != ERROR_SUCCESS) return s;
    }
    return ERROR_SUCCESS;
}

LSTATUS unregister_extension(HKEY root, const wchar_t* ext) {
    const std::wstring base = std::wstring(L"Software\\Classes\\") + ext;
    delete_tree(root, base + L"\\" + kShellExPreview);
    delete_tree(root, base + L"\\" + kShellExThumbnail);
    return ERROR_SUCCESS;
}

}  // namespace

HRESULT RegisterShellExtensions() {
    const HKEY root = HKEY_LOCAL_MACHINE;

    LSTATUS s;
    s = register_clsid(root, CLSID_FitsPreviewHandler,
                       L"FITS Preview Handler", L"Apartment",
                       APPID_FitsPreviewSurrogate);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    write_dword(root,
        std::wstring(kClsIdRoot) + L"\\" + guid_to_string(CLSID_FitsPreviewHandler),
        L"DisableLowILProcessIsolation", 1);

    s = register_clsid(root, CLSID_FitsPropertyHandler,
                       L"FITS Property Handler", L"Apartment",
                       GUID_NULL);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);

    s = register_clsid(root, CLSID_FitsThumbnailProvider,
                       L"FITS Thumbnail Provider", L"Apartment",
                       APPID_FitsThumbnailSurrogate);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);

    // Global PreviewHandlers list
    write_string(root, kPreviewHandlersList,
                 guid_to_string(CLSID_FitsPreviewHandler).c_str(),
                 L"FITS Preview Handler");

    // Property + preview + thumbnail handlers for every supported extension.
    for (const wchar_t* ext : kAllExts) {
        std::wstring k = std::wstring(kPropHandlersBase) + L"\\" + ext;
        write_string(root, k, nullptr, guid_to_string(CLSID_FitsPropertyHandler));
        register_extension(root, ext,
                           CLSID_FitsPreviewHandler, CLSID_FitsThumbnailProvider);
    }

    // ProgID: lets Explorer pick up our icon and open .fit/.fits with the
    // viewer. Only registered if WinStellar.exe was staged alongside the DLL.
    {
        const std::wstring exe = viewer_exe_path();
        DWORD attr = ::GetFileAttributesW(exe.c_str());
        if (exe.size() && attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring progBase = std::wstring(L"Software\\Classes\\") + kProgId;
            write_string(root, progBase, nullptr, L"FITS Image");
            write_string(root, progBase + L"\\DefaultIcon", nullptr, exe + L",0");
            const std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
            write_string(root, progBase + L"\\shell\\open\\command", nullptr, cmd);
            // FriendlyAppName for the Open With UI
            write_string(root, progBase + L"\\Application", L"ApplicationName", L"WinStellar");

            // List WinStellar in the Open With menu for every supported
            // extension; make it the *default* opener only for the formats
            // Windows has no handler for (never hijack RAW's default app).
            for (const wchar_t* ext : kAllExts) {
                const std::wstring extKey = std::wstring(L"Software\\Classes\\") + ext;
                write_string(root, extKey + L"\\OpenWithProgids", kProgId, L"");
            }
            for (const wchar_t* ext : kDefaultAssocExts) {
                const std::wstring extKey = std::wstring(L"Software\\Classes\\") + ext;
                write_string(root, extKey, nullptr, kProgId);
            }
        }
    }

    // Property schema (.propdesc next to the DLL).
    // Force-refresh first to drop any cached schema from a previous register
    // attempt (otherwise PropertyDescriptions can be stale across rebuilds).
    {
        std::wstring p = module_path();
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            p = p.substr(0, slash + 1) + L"FitsProps.propdesc";
            ::PSUnregisterPropertySchema(p.c_str());
            HRESULT hr = ::PSRegisterPropertySchema(p.c_str());
            if (FAILED(hr)) {
                wchar_t msg[256];
                swprintf_s(msg, L"PSRegisterPropertySchema failed: 0x%08lX (path: %s)", hr, p.c_str());
                ::OutputDebugStringW(msg);
                // Surface to the caller so regsvr32 reports failure too.
                ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
                return hr;
            }
        }
    }

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterShellExtensions() {
    const HKEY root = HKEY_LOCAL_MACHINE;

    // Property schema first (so the file can stay on disk and be re-registered)
    {
        std::wstring p = module_path();
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            p = p.substr(0, slash + 1) + L"FitsProps.propdesc";
            ::PSUnregisterPropertySchema(p.c_str());
        }
    }

    // Extension associations
    for (const wchar_t* ext : kAllExts)
        unregister_extension(root, ext);

    // ProgID + bindings
    delete_tree(root, std::wstring(L"Software\\Classes\\") + kProgId);
    for (const wchar_t* ext : kAllExts) {
        const std::wstring extKey = std::wstring(L"Software\\Classes\\") + ext;
        // Remove our entry from OpenWithProgids
        ::RegDeleteKeyValueW(root, (extKey + L"\\OpenWithProgids").c_str(), kProgId);
        // If we own the default value, clear it. (Don't blow away whatever
        // another app might have set after us.)
        HKEY h = nullptr;
        if (::RegOpenKeyExW(root, extKey.c_str(), 0, KEY_READ | KEY_WRITE, &h) == ERROR_SUCCESS) {
            wchar_t buf[256] = {};
            DWORD type = 0, sz = sizeof(buf);
            if (::RegQueryValueExW(h, nullptr, nullptr, &type,
                                   reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS &&
                type == REG_SZ && _wcsicmp(buf, kProgId) == 0) {
                ::RegDeleteValueW(h, nullptr);
            }
            ::RegCloseKey(h);
        }
    }

    // Property handler associations
    for (const wchar_t* ext : kAllExts)
        delete_tree(root, std::wstring(kPropHandlersBase) + L"\\" + ext);

    // PreviewHandlers entry
    ::RegDeleteKeyValueW(root, kPreviewHandlersList,
                         guid_to_string(CLSID_FitsPreviewHandler).c_str());

    // CLSID trees
    delete_tree(root, std::wstring(kClsIdRoot) + L"\\" + guid_to_string(CLSID_FitsPreviewHandler));
    delete_tree(root, std::wstring(kClsIdRoot) + L"\\" + guid_to_string(CLSID_FitsPropertyHandler));
    delete_tree(root, std::wstring(kClsIdRoot) + L"\\" + guid_to_string(CLSID_FitsThumbnailProvider));

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    return S_OK;
}
