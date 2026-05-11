@echo off
pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev_cycle.ps1" %*
popd
