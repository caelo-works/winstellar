# WinStellar

A native Windows viewer for FITS and XISF astrophotography files, with deep
Windows Explorer integration so you can triage subs without launching anything.

- **Preview pane** (Alt+P) — auto-stretched render of the selected file
- **Details pane** — FITS keywords (object, filter, gain, exposure, focal
  length, …) and computed image-quality metrics (HFR, FWHM, eccentricity,
  star count, pixel stats) as native Windows properties
- **Thumbnails** in Explorer's icon view
- **Standalone viewer** with zoom / pan / rotate, dark-mode UI, and a
  fast SQLite-backed analysis cache so re-browsing a folder is instant

Supported formats: `.fit`, `.fits`, `.xisf` (PixInsight).

---

## Install

Download the latest installer from the
[Releases page](https://github.com/caelo-works/winstellar/releases) and run
it. Windows 10 or 11, 64-bit only.

The installer registers the shell extensions, restarts Explorer, and adds an
entry to **Apps & features** for clean uninstall.

> If Microsoft SmartScreen warns you that the publisher is unverified, that
> is because the binary is signed with a standard (not Extended Validation)
> certificate that hasn't yet built reputation. Click **More info → Run
> anyway**. The warning goes away once enough people have installed the
> release.

After install, open any folder containing `.fit` / `.fits` / `.xisf` files
in Explorer, add the WinStellar columns (right-click on a column header →
*More…* → search for *HFR*, *Stars*, *Object*…), and sort by HFR to surface
your sharpest subs.

## Screenshots

*(screenshots to be added)*

## Build from source

If you'd rather build it yourself, the toolchain is MSVC + CMake + vcpkg.

**Prerequisites** (one-time, on the Windows host):

1. **Visual Studio 2022** with the *Desktop development with C++* workload
   (provides MSVC v143 and the Windows 11 SDK).
2. **vcpkg** — clone to `C:\dev\vcpkg`, run `bootstrap-vcpkg.bat`, then set
   `VCPKG_ROOT=C:\dev\vcpkg` in your user environment variables.
3. **CMake ≥ 3.21** and **Ninja** (both ship with the VS C++ workload).
4. **Inno Setup 6** (only needed if you want to build the installer):
   `winget install --id JRSoftware.InnoSetup`

Run `scripts\check_env.cmd` to validate the toolchain is wired up.

**Build:**

```powershell
.\scripts\build.cmd -Config Release
```

Outputs land in `build\bin\Release\` :

- `WinStellar.exe` — the standalone viewer
- `WinStellarShellExt.dll` — the Explorer shell extensions DLL

**Register the shell extension** (for testing without the installer):

```powershell
.\scripts\register.cmd       # admin required (auto-elevates)
.\scripts\unregister.cmd     # to undo
```

**Build an installer:**

```powershell
.\installer\build_installer.cmd                       # unsigned
.\installer\build_installer.cmd -SigningCert <THUMB>  # signed
```

## Release process

For maintainers cutting a new version:

```powershell
.\scripts\release.cmd -Patch                    # 0.1.2 -> 0.1.3
.\scripts\release.cmd -Minor                    # 0.1.2 -> 0.2.0
.\scripts\release.cmd -Major -Push              # 0.1.2 -> 1.0.0 + git push
.\scripts\release.cmd -Set 1.2.3 -SigningCert <THUMB>
```

The script bumps `CMakeLists.txt` + `vcpkg.json`, commits, tags `vX.Y.Z`,
rebuilds, and packages the installer. Pass `-Push` to publish.

The VERSIONINFO of every binary embeds the version, git short SHA, and
build date — visible from Properties → Details on the `.exe`/`.dll`.

## Project layout

```
fits_core/    Static library — FITS / XISF loading, stretch, analysis
shell_ext/    Preview / Property / Thumbnail handlers (a single COM DLL)
viewer/       Standalone Win32 + Direct2D viewer
scripts/      PowerShell helpers (build, register, release, version bump)
installer/    Inno Setup script + signing pipeline
tools/        Side utilities (XISF benchmark)
```

## License

GPLv3 — see [LICENSE](LICENSE).

## Acknowledgements

- [CFITSIO](https://heasarc.gsfc.nasa.gov/fitsio/) for FITS I/O
- [pugixml](https://pugixml.org/) for parsing XISF XML headers
- [SQLite](https://sqlite.org/) for the analysis cache
- PixInsight's AutoSTF algorithm for the auto-stretch approach
