#include "UpdateCheck.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <wintrust.h>
#include <bcrypt.h>

#include <cstdio>
#include <string>
#include <vector>

namespace wsu {

namespace {

constexpr const wchar_t* kRegKey    = L"Software\\WinStellar";
constexpr const wchar_t* kUserAgent = L"WinStellar-Updater";
constexpr const wchar_t* kHost      = L"github.com";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

// Split "https://host/path" into host and path. Anything that is not https is
// rejected outright -- the updater never speaks plaintext.
bool split_https(const std::string& url, std::wstring& host, std::wstring& path) {
    constexpr const char* kScheme = "https://";
    if (url.compare(0, 8, kScheme) != 0) return false;
    const size_t slash = url.find('/', 8);
    if (slash == std::string::npos) return false;
    host = widen(url.substr(8, slash - 8));
    path = widen(url.substr(slash));
    return !host.empty() && !path.empty();
}

struct Session {
    HINTERNET h = nullptr;
    ~Session() { if (h) ::WinHttpCloseHandle(h); }
};

// One request. `follow` controls redirects: the latest-version probe must NOT
// follow, because the redirect target IS the answer; asset downloads must,
// because GitHub bounces them to its object storage.
bool http_get(const std::wstring& host, const std::wstring& path, bool follow,
              std::vector<uint8_t>* body, std::wstring* location, DWORD* status_out) {
    Session sess;
    sess.h = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess.h) return false;

    // Modest timeouts: an update check must never make the application feel slow.
    ::WinHttpSetTimeouts(sess.h, 10000, 10000, 20000, 60000);

    HINTERNET conn = ::WinHttpConnect(sess.h, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) return false;

    HINTERNET req = ::WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!req) { ::WinHttpCloseHandle(conn); return false; }

    if (!follow) {
        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        ::WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    }

    bool ok = false;
    if (::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(req, nullptr)) {

        DWORD status = 0, len = sizeof(status);
        ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status_out) *status_out = status;

        if (location) {
            DWORD n = 0;
            ::WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                  nullptr, &n, WINHTTP_NO_HEADER_INDEX);
            if (n > 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::wstring buf(n / sizeof(wchar_t), L'\0');
                if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                          buf.data(), &n, WINHTTP_NO_HEADER_INDEX)) {
                    buf.resize(wcslen(buf.c_str()));
                    *location = buf;
                }
            }
        }

        if (body) {
            body->clear();
            for (;;) {
                DWORD avail = 0;
                if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                const size_t at = body->size();
                // A release asset is a few MB; refuse anything absurd rather than
                // let a hostile response exhaust memory.
                if (at + avail > 300u * 1024u * 1024u) { body->clear(); break; }
                body->resize(at + avail);
                DWORD read = 0;
                if (!::WinHttpReadData(req, body->data() + at, avail, &read)) { body->clear(); break; }
                body->resize(at + read);
            }
        }
        ok = (status == 200) || (!follow && status >= 300 && status < 400);
    }

    ::WinHttpCloseHandle(req);
    ::WinHttpCloseHandle(conn);
    return ok;
}

std::wstring updates_dir() {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) {
        dir = std::wstring(base) + L"\\WinStellar\\updates";
        ::CoTaskMemFree(base);
        ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    return dir;
}

std::string sha256_of(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (::BCryptHashData(h, const_cast<PUCHAR>(data.data()),
                             static_cast<ULONG>(data.size()), 0) == 0) {
            UCHAR digest[32] = {};
            if (::BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
                char hex[65] = {};
                for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
                out.assign(hex, 64);
            }
        }
        ::BCryptDestroyHash(h);
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = ::WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr)
                 && wrote == data.size();
    ::CloseHandle(f);
    return ok;
}

SignatureState verify_authenticode(const std::wstring& path) {
    WINTRUST_FILE_INFO file = {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG;

    const LONG rc = ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);

    if (rc == ERROR_SUCCESS) return SignatureState::Valid;
    // No signature at all is a different situation from a broken one: the former
    // is where we are today, the latter is always an attack or a corrupt file.
    if (rc == TRUST_E_NOSIGNATURE || rc == static_cast<LONG>(0x800B0100)) return SignatureState::Unsigned;
    return SignatureState::Invalid;
}

bool reg_get_dword(const wchar_t* name, DWORD* out) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = sizeof(DWORD);
    const LSTATUS rc = ::RegQueryValueExW(k, name, nullptr, &type,
                                          reinterpret_cast<BYTE*>(out), &cb);
    ::RegCloseKey(k);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

void reg_put_dword(const wchar_t* name, DWORD v) {
    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    ::RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    ::RegCloseKey(k);
}

DWORD today_stamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return st.wYear * 10000u + st.wMonth * 100u + st.wDay;
}

}  // namespace

UpdateCheckResult check_for_update(const Version& current) {
    UpdateCheckResult out;

    std::wstring location;
    DWORD status = 0;
    if (!http_get(kHost, L"/caelo-works/winstellar/releases/latest",
                  /*follow=*/false, nullptr, &location, &status) || location.empty()) {
        out.error = "Could not reach the update server.";
        return out;
    }

    // The redirect target is attacker-influenceable in a way nothing else here
    // is, so it is matched against our own repository rather than merely parsed.
    const std::string tag = tag_from_release_url(narrow(location));
    if (tag.empty()) {
        out.error = "The update server returned an unexpected address.";
        return out;
    }

    const Version latest = parse_version(tag);
    if (!latest.valid()) {
        out.error = "The update server returned an unreadable version.";
        return out;
    }

    out.tag = tag;
    out.version = latest;
    out.available = is_newer(latest, current);   // never true for a downgrade
    return out;
}

InstallResult download_and_verify(const UpdateCheckResult& r) {
    InstallResult out;
    if (!r.available || r.tag.empty()) {
        out.error = L"There is no update to download.";
        return out;
    }

    const std::string name = installer_name(r.version);

    // The published checksum list, fetched first: without it there is nothing to
    // check the installer against and the download is refused outright.
    std::wstring h, p;
    std::vector<uint8_t> sums_body;
    if (!split_https(sums_url(r.tag), h, p) ||
        !http_get(h, p, /*follow=*/true, &sums_body, nullptr, nullptr) || sums_body.empty()) {
        out.error = L"The published checksums could not be downloaded, so the update was "
                    L"not installed.";
        return out;
    }
    const std::string expected =
        sha256_for(std::string(sums_body.begin(), sums_body.end()), name);
    if (!looks_like_sha256(expected)) {
        out.error = L"The published checksums do not cover this installer, so the update "
                    L"was not installed.";
        return out;
    }

    std::vector<uint8_t> exe;
    if (!split_https(installer_url(r.tag, r.version), h, p) ||
        !http_get(h, p, /*follow=*/true, &exe, nullptr, nullptr) || exe.empty()) {
        out.error = L"The installer could not be downloaded.";
        return out;
    }

    if (sha256_of(exe) != expected) {
        out.error = L"The downloaded installer does not match its published checksum. "
                    L"It was discarded and nothing was installed.";
        return out;
    }

    const std::wstring dir = updates_dir();
    if (dir.empty()) {
        out.error = L"No writable location was found for the download.";
        return out;
    }
    const std::wstring path = dir + L"\\" + widen(name);
    if (!write_file(path, exe)) {
        out.error = L"The installer could not be saved to disk.";
        return out;
    }

    out.signature = verify_authenticode(path);
    if (out.signature == SignatureState::Invalid ||
        (kRequireSignature && out.signature != SignatureState::Valid)) {
        ::DeleteFileW(path.c_str());
        out.error = L"The downloaded installer's signature could not be verified. "
                    L"It was discarded and nothing was installed.";
        return out;
    }

    out.success = true;
    out.installer_path = path;
    return out;
}

bool launch_installer(const std::wstring& path) {
    // "runas": the installer needs administrator rights to touch Program Files
    // and re-register the Explorer extensions. The user sees the UAC prompt.
    SHELLEXECUTEINFOW ei = {};
    ei.cbSize = sizeof(ei);
    ei.fMask = SEE_MASK_NOASYNC;
    ei.lpVerb = L"runas";
    ei.lpFile = path.c_str();
    ei.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&ei) != FALSE;
}

bool update_check_enabled() {
    DWORD v = 1;                       // on by default: the point is to reach
    reg_get_dword(L"UpdateCheck", &v); // people who would never go looking
    return v != 0;
}

void set_update_check_enabled(bool on) { reg_put_dword(L"UpdateCheck", on ? 1u : 0u); }

bool should_check_today() {
    DWORD last = 0;
    if (!reg_get_dword(L"UpdateLastCheck", &last)) return true;
    return last != today_stamp();      // at most one check per day
}

void mark_checked_today() { reg_put_dword(L"UpdateLastCheck", today_stamp()); }

}  // namespace wsu
