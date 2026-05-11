@echo off
pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0release.ps1" %*
popd
pause
