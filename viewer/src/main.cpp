#include <windows.h>
#include <shellapi.h>

#include "ViewerWindow.h"

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
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
