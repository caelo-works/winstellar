#pragma once

#include "UpdateInfo.h"

#include <string>

namespace wsu {

// Update check and install.
//
// This downloads an executable and asks Windows to run it elevated, so it is
// treated as a security boundary throughout: the tag comes from a redirect that
// must match our own repository, the download URL is BUILT from that tag rather
// than taken from any response, the file is checked against the published
// SHA-256, and its Authenticode signature is verified when it has one.

enum class SignatureState {
    Unsigned,   // no Authenticode signature at all
    Valid,      // signed, and Windows trusts the chain
    Invalid,    // signed, but the signature does not verify -- always fatal
};

// WinStellar's installer is not code-signed yet. Until it is, requiring a valid
// signature would make the updater permanently inert, so an unsigned installer is
// accepted and the real guarantees are TLS plus the published checksum. Flip this
// to true the day a certificate exists: nothing else has to change.
inline constexpr bool kRequireSignature = false;

struct UpdateCheckResult {
    bool        available = false;   // a strictly newer version was published
    Version     version{};
    std::string tag;
    std::string error;               // empty unless the check itself failed
};

// Ask the release server what the latest published version is. Network call;
// never run this on the UI thread. Returns available=false (not an error) when
// the running build is already current.
[[nodiscard]] UpdateCheckResult check_for_update(const Version& current);

struct InstallResult {
    bool           success = false;
    SignatureState signature = SignatureState::Unsigned;
    std::wstring   installer_path;
    std::wstring   error;            // a finished English sentence
};

// Download the installer for `r`, verify it, and leave it ready to run. Does NOT
// launch anything. Network + hashing; worker thread only.
[[nodiscard]] InstallResult download_and_verify(const UpdateCheckResult& r);

// Hand the verified installer to Windows with elevation. Returns false if the
// user declined the UAC prompt or the shell refused. The caller is expected to
// close the application afterwards so the installer can replace its files.
[[nodiscard]] bool launch_installer(const std::wstring& path);

// Whether the automatic check is enabled, and whether it already ran today.
// Stored next to the other viewer settings in HKCU\Software\WinStellar.
[[nodiscard]] bool update_check_enabled();
void set_update_check_enabled(bool on);
[[nodiscard]] bool should_check_today();
void mark_checked_today();

}  // namespace wsu
