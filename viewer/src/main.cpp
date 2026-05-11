#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>

#include "ViewerWindow.h"
#include "fits_core/fits_loader.h"
#include "fits_core/analysis.h"

namespace {

// Headless self-check used by the CI smoke test: load the given file, run the
// full analysis pipeline, print a one-line summary to stdout, exit 0 on
// success / 1 on failure. No window is created, so the call returns quickly
// and doesn't require a display.
int run_check(const wchar_t* path) {
    if (!path || !*path) {
        std::fwprintf(stderr, L"--check requires a file path\n");
        return 1;
    }
    auto loaded = fitsx::load_from_file(path);
    if (!loaded.success) {
        std::fwprintf(stderr, L"load failed: %hs\n", loaded.error.c_str());
        return 1;
    }
    auto ar = fitsx::run_analysis(loaded.image);
    std::wprintf(L"OK %dx%d headers=%zu min=%.3f max=%.3f stars=%d hfr=%.2f\n",
                 loaded.image.width, loaded.image.height,
                 loaded.image.headers.size(),
                 loaded.image.source_min, loaded.image.source_max,
                 ar.star_count, ar.hfr_median);
    return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    // --check <file>: headless validation path used by CI / scripting; never
    // opens a window. Stay parse-tolerant — argv[0] is always the exe path.
    if (argc >= 3 && std::wcscmp(argv[1], L"--check") == 0) {
        // Attach to parent console (if launched from a terminal) so wprintf
        // output is visible — Windows GUI subsystem detaches stdio by default.
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        const int rc = run_check(argv[2]);
        if (argv) ::LocalFree(argv);
        ::CoUninitialize();
        return rc;
    }

    const wchar_t* initial = (argc >= 2) ? argv[1] : nullptr;

    ViewerWindow win;
    int rc = 1;
    if (win.create(hinst, initial)) {
        rc = win.run_message_loop();
    }

    if (argv) ::LocalFree(argv);
    ::CoUninitialize();
    return rc;
}
