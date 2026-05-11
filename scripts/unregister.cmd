@echo off
NET FILE 1>NUL 2>NUL
if not '%errorlevel%' == '0' (
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0unregister.ps1" %*
echo.
echo --- script finished, exit code %errorlevel% ---
pause
popd
