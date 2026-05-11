#include <gtest/gtest.h>

#include <windows.h>

// WINSTELLAR_SHELL_EXT_PATH is baked at CMake time via a generator
// expression — it resolves to the absolute path of the built DLL for the
// current configuration (Release/Debug). Stringizing isn't enough because
// the build script puts a literal "..." around the value already.
#ifndef WINSTELLAR_SHELL_EXT_PATH
#  error "WINSTELLAR_SHELL_EXT_PATH must be defined by CMake"
#endif

namespace {

// Helper: load the DLL, run a closure with it, free. Returns final assertion
// outcome — wrapped so multiple tests stay isolated (no shared HMODULE).
template <class Fn>
void with_dll(Fn&& fn) {
    HMODULE h = ::LoadLibraryA(WINSTELLAR_SHELL_EXT_PATH);
    ASSERT_NE(h, nullptr)
        << "LoadLibrary failed for " << WINSTELLAR_SHELL_EXT_PATH
        << ", GetLastError=" << ::GetLastError();
    fn(h);
    ::FreeLibrary(h);
}

}  // namespace

TEST(ShellExt, LoadsAsLibrary) {
    with_dll([](HMODULE) { /* loaded successfully */ });
}

TEST(ShellExt, ExportsDllRegisterServer) {
    with_dll([](HMODULE h) {
        EXPECT_NE(::GetProcAddress(h, "DllRegisterServer"), nullptr);
    });
}

TEST(ShellExt, ExportsDllUnregisterServer) {
    with_dll([](HMODULE h) {
        EXPECT_NE(::GetProcAddress(h, "DllUnregisterServer"), nullptr);
    });
}

TEST(ShellExt, ExportsDllCanUnloadNow) {
    with_dll([](HMODULE h) {
        auto fn = ::GetProcAddress(h, "DllCanUnloadNow");
        ASSERT_NE(fn, nullptr);
        // No references taken yet, so it should report S_OK (can unload).
        using DllCanUnloadNowFn = HRESULT (STDAPICALLTYPE*)();
        HRESULT hr = reinterpret_cast<DllCanUnloadNowFn>(fn)();
        EXPECT_EQ(hr, S_OK);
    });
}

TEST(ShellExt, ExportsDllGetClassObject) {
    with_dll([](HMODULE h) {
        EXPECT_NE(::GetProcAddress(h, "DllGetClassObject"), nullptr);
    });
}
