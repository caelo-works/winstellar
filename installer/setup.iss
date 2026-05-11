; WinStellar installer (Inno Setup 6)
;
; Builds a single-exe installer that:
;   - copies all binaries to %ProgramFiles%\WinStellar
;   - registers the shell extensions (regsvr32 on WinStellarShellExt.dll)
;   - restarts Explorer so handlers load
;   - adds a Start Menu shortcut + an "Apps & features" entry
;   - cleanly uninstalls (unregister + restart Explorer + delete files)
;
; Build with:  installer\build_installer.cmd
; Output:      build\installer\WinStellarSetup-<version>.exe

#define MyAppId         "{{B7E2A4F1-3B5C-4D8E-9F1A-B2C3D4E5F901}"
#define MyAppName       "WinStellar"
; MyAppVersion is provided by build_installer.ps1 via /DMyAppVersion=...
; Keep a fallback so opening the .iss in the IDE still compiles.
#ifndef MyAppVersion
  #define MyAppVersion  "0.0.0-dev"
#endif
#define MyAppPublisher  "Caelo Works"

; If build_installer.ps1 runs with -SigningCert, it passes /DSign=1 plus
; /Ssigntool=... so Inno Setup signs every staged binary AND the final
; setup.exe. Without a cert the produced installer is unsigned.
#ifdef Sign
  #define SignFlag " sign"
#else
  #define SignFlag ""
#endif
#define MyAppURL        "https://github.com/caelo-works/winstellar"
#define MyAppExeName    "WinStellar.exe"
#define BuildDir        "..\build\bin\Release"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\build\installer
OutputBaseFilename=WinStellarSetup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
SetupIconFile=..\viewer\assets\winstellar.ico
CloseApplications=force
RestartApplications=no
#ifdef Sign
SignTool=signtool
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
  GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable + shell extension DLL
Source: "{#BuildDir}\{#MyAppExeName}";          DestDir: "{app}"; Flags: ignoreversion{#SignFlag}
Source: "{#BuildDir}\WinStellarShellExt.dll";   DestDir: "{app}"; Flags: ignoreversion{#SignFlag} regserver{#SignFlag}
Source: "{#BuildDir}\FitsProps.propdesc";       DestDir: "{app}"; Flags: ignoreversion

; Vendored DLLs from vcpkg (they live next to the exe/dll for SxS load)
Source: "{#BuildDir}\cfitsio.dll";        DestDir: "{app}"; Flags: ignoreversion{#SignFlag}
Source: "{#BuildDir}\sqlite3.dll";        DestDir: "{app}"; Flags: ignoreversion{#SignFlag}
Source: "{#BuildDir}\pugixml.dll";        DestDir: "{app}"; Flags: ignoreversion{#SignFlag}
Source: "{#BuildDir}\z.dll";              DestDir: "{app}"; Flags: ignoreversion{#SignFlag}
; pthreads-win32 is a transitive dep of cfitsio (REENTRANT build for thread-safe loads)
Source: "{#BuildDir}\pthreadVC3.dll";     DestDir: "{app}"; Flags: ignoreversion{#SignFlag}

[Icons]
Name: "{group}\{#MyAppName}";   Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Bounce Explorer so the freshly registered shell extensions are picked up
; immediately instead of "after next reboot".
Filename: "{cmd}"; Parameters: "/c taskkill /F /IM explorer.exe & start explorer.exe"; \
  Flags: runhidden waituntilterminated; \
  StatusMsg: "Restarting Windows Explorer..."

; Optional: launch the viewer at the end of the installer
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop Windows Search service so it releases the property handler DLL.
Filename: "{cmd}"; Parameters: "/c net stop WSearch /y & taskkill /F /IM explorer.exe"; \
  Flags: runhidden waituntilterminated; RunOnceId: "ReleaseHandlers"

; The shell extension DLL is unregistered automatically because we used the
; 'regserver' flag in [Files] (Inno Setup calls DllUnregisterServer at uninstall).

; Restart Explorer + Search after files are removed.
Filename: "{cmd}"; Parameters: "/c net start WSearch & start explorer.exe"; \
  Flags: runhidden waituntilterminated; RunOnceId: "ResumeHandlers"

[UninstallDelete]
; Wipe analysis cache + any lingering staged files from old dev installs
; (legacy %LOCALAPPDATA%\FitsViewer kept for users upgrading from pre-rename builds).
Type: filesandordirs; Name: "{localappdata}\WinStellar"
Type: filesandordirs; Name: "{localappdata}\FitsViewer"

[Code]
function InitializeSetup(): Boolean;
var
  Version: TWindowsVersion;
begin
  Result := True;
  GetWindowsVersionEx(Version);
  if Version.NTPlatform and (Version.Major < 10) then begin
    MsgBox('Windows 10 or later is required.', mbError, MB_OK);
    Result := False;
  end;
end;
