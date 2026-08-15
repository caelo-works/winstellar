#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>

#include "ImageExport.h"
#include "UpdateCheck.h"
#include "ViewerWindow.h"
#include "fits_core/fits_loader.h"
#include "fits_core/fits_stretch.h"
#include "fits_core/analysis.h"
#include "fits_version.h"

#include <memory>
#include <string>
#include <thread>

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
    try {
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
    } catch (const std::exception& e) {
        std::fwprintf(stderr, L"load failed (exception): %hs\n", e.what());
        return 1;
    } catch (...) {
        std::fwprintf(stderr, L"load failed (unknown exception)\n");
        return 1;
    }
}

// Headless export, used by the smoke test and scriptable by anyone:
//   WinStellar.exe --export <jpg|png|tif|fits> <in> <out>
//                  [--rot 0|90|180|270] [--stretch auto|none] [--quality 75..100]
// Exits 0 on success, 1 on failure.
int run_export_cli(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::fwprintf(stderr, L"--export requires <format> <input> <output>\n");
        return 1;
    }
    const std::wstring fmt_s = argv[2];
    wsx::Format fmt;
    if      (fmt_s == L"jpg" || fmt_s == L"jpeg") fmt = wsx::Format::Jpeg;
    else if (fmt_s == L"png")                     fmt = wsx::Format::Png;
    else if (fmt_s == L"tif" || fmt_s == L"tiff") fmt = wsx::Format::Tiff;
    else if (fmt_s == L"fits" || fmt_s == L"fit") fmt = wsx::Format::Fits;
    else {
        std::fwprintf(stderr, L"unknown export format: %s\n", fmt_s.c_str());
        return 1;
    }

    int rot = 0, quality = 95;
    bool auto_stretch = true;
    for (int i = 5; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--rot") == 0 && i + 1 < argc)
            rot = _wtoi(argv[++i]);
        else if (std::wcscmp(argv[i], L"--stretch") == 0 && i + 1 < argc)
            auto_stretch = (std::wcscmp(argv[++i], L"none") != 0);
        else if (std::wcscmp(argv[i], L"--quality") == 0 && i + 1 < argc)
            quality = _wtoi(argv[++i]);
    }

    auto loaded = fitsx::load_from_file(argv[3]);
    if (!loaded.success) {
        std::fwprintf(stderr, L"load failed: %hs\n", loaded.error.c_str());
        return 1;
    }

    wsx::ExportRequest req;
    req.format = fmt;
    req.dest_path = argv[4];
    req.source_path = argv[3];
    req.image = std::make_shared<const fitsx::FitsImage>(std::move(loaded.image));
    req.stretch = auto_stretch ? fitsx::compute_auto_stretch(*req.image)
                               : fitsx::StretchParams{};
    req.display_rotation_deg = rot;
    req.jpeg_quality = quality;
    req.allow_memory_fits_fallback = true;
    req.app_version = FITS_VERSION_STR;

    // Deliberately a real spawned thread. wWinMain has already put THIS thread
    // into an STA, so running inline would prove nothing about the export
    // worker -- whose most likely runtime failure is CO_E_NOTINITIALIZED on a
    // thread with no COM at all.
    wsx::ExportOutcome outcome;
    std::thread t([&] {
        wsx::ComApartment com;
        if (!com.ok()) {
            outcome.error = L"COM could not be initialised on the export thread.";
            return;
        }
        outcome = wsx::run_export(req, [] { return false; });
    });
    t.join();

    if (!outcome.success) {
        std::fwprintf(stderr, L"export failed: %s\n",
                      outcome.error.empty() ? L"(no reason given)" : outcome.error.c_str());
        return 1;
    }
    std::wprintf(L"OK %s\n", req.dest_path.c_str());
    return 0;
}

// Headless update check:
//   WinStellar.exe --check-update [X.Y.Z] [--download]
// The optional version is treated as the running one, so the "an update exists"
// path can be exercised without waiting for a newer release -- useful for tests
// and for answering "what would a 0.7.0 user see?". --download additionally
// fetches and verifies the installer but never runs it.
// Exit 0 = up to date, 10 = update available (and verified if --download),
// 1 = the check or the verification failed.
int run_update_check_cli(int argc, wchar_t** argv) {
    wsu::Version current = wsu::parse_version(FITS_VERSION_STR);
    bool download = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--download") == 0) { download = true; continue; }
        char buf[64] = {};
        ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buf, sizeof(buf) - 1, nullptr, nullptr);
        const wsu::Version v = wsu::parse_version(buf);
        if (v.valid()) current = v;
    }

    std::wprintf(L"current %d.%d.%d\n", current.major, current.minor, current.patch);
    const wsu::UpdateCheckResult r = wsu::check_for_update(current);
    if (!r.error.empty()) {
        std::fwprintf(stderr, L"check failed: %hs\n", r.error.c_str());
        return 1;
    }
    std::wprintf(L"latest %d.%d.%d (%hs)\n", r.version.major, r.version.minor,
                 r.version.patch, r.tag.c_str());
    if (!r.available) {
        std::wprintf(L"up to date\n");
        return 0;
    }
    std::wprintf(L"update available\n");
    if (!download) return 10;

    const wsu::InstallResult ir = wsu::download_and_verify(r);
    if (!ir.success) {
        std::fwprintf(stderr, L"download/verify failed: %s\n", ir.error.c_str());
        return 1;
    }
    std::wprintf(L"verified %s\nsignature %s\n", ir.installer_path.c_str(),
                 ir.signature == wsu::SignatureState::Valid    ? L"valid"
               : ir.signature == wsu::SignatureState::Unsigned ? L"unsigned"
                                                               : L"invalid");
    return 10;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    // --check <file>: headless validation path used by CI / scripting; never
    // opens a window. Stay parse-tolerant — argv[0] is always the exe path.
    if (argc >= 2 && std::wcscmp(argv[1], L"--check-update") == 0) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        const int rc = run_update_check_cli(argc, argv);
        if (argv) ::LocalFree(argv);
        ::CoUninitialize();
        return rc;
    }

    if (argc >= 2 && std::wcscmp(argv[1], L"--export") == 0) {
        if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        const int rc = run_export_cli(argc, argv);
        if (argv) ::LocalFree(argv);
        ::CoUninitialize();
        return rc;
    }

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
