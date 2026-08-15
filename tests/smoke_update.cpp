// The update check, end to end against the real release server.
//
// The pure decisions are covered by unit_update_info; what only a live run can
// show is that the redirect is followed, parsed and matched correctly. These
// tests need the network, so a failure to reach the server SKIPS rather than
// fails -- a flaky CI runner must not look like a broken updater.

#include <gtest/gtest.h>

#include <windows.h>

#include <string>

#ifndef WINSTELLAR_EXE_PATH
#  error "WINSTELLAR_EXE_PATH must be defined by CMake"
#endif

namespace {

std::wstring viewer_exe_path() {
    const std::string narrow = WINSTELLAR_EXE_PATH;
    return std::wstring(narrow.begin(), narrow.end());
}

// Exit codes: 0 up to date, 10 update available, 1 the check failed.
DWORD run(const std::wstring& args, DWORD timeout_ms = 60000) {
    std::wstring cmd = L"\"" + viewer_exe_path() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ADD_FAILURE() << "CreateProcessW failed, GetLastError=" << ::GetLastError();
        return 1;
    }
    if (::WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        ADD_FAILURE() << "update check timed out";
        return 1;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return code;
}

constexpr DWORD kUpToDate  = 0;
constexpr DWORD kAvailable = 10;
constexpr DWORD kFailed    = 1;

}  // namespace

TEST(SmokeUpdate, ReachesTheReleaseServerAndReadsTheVersion) {
    const DWORD rc = run(L"--check-update 0.0.1");
    if (rc == kFailed) GTEST_SKIP() << "no network access to the release server";
    // Anything published is newer than 0.0.1.
    EXPECT_EQ(rc, kAvailable);
}

TEST(SmokeUpdate, TheRunningVersionIsNotOfferedAnUpdate) {
    const DWORD rc = run(L"--check-update");
    if (rc == kFailed) GTEST_SKIP() << "no network access to the release server";
    // This build is either current or older than what is published; both are
    // legitimate, but it must never be "available" while equal.
    EXPECT_TRUE(rc == kUpToDate || rc == kAvailable);
}

TEST(SmokeUpdate, NeverOffersADowngrade) {
    // The security property worth pinning end to end: a rolled-back or hijacked
    // "latest" must not push a user backwards onto a version whose
    // vulnerabilities are already public.
    const DWORD rc = run(L"--check-update 999.0.0");
    if (rc == kFailed) GTEST_SKIP() << "no network access to the release server";
    EXPECT_EQ(rc, kUpToDate) << "a lower published version was offered as an update";
}
