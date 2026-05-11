@echo off
pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0create_dev_cert.ps1" %*
popd
