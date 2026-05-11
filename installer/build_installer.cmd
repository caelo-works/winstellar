@echo off
pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_installer.ps1" %*
echo.
echo --- finished, exit %errorlevel% ---
pause
popd
