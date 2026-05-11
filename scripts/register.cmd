@echo off
:: Self-elevate so register.ps1 always runs in a single admin process. This
:: avoids the auto-elevation gymnastics that lose stdout when the elevated PS
:: window closes too fast.
NET FILE 1>NUL 2>NUL
if not '%errorlevel%' == '0' (
    powershell -NoProfile -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

pushd "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0register.ps1" %*
echo.
echo --- script finished, exit code %errorlevel% ---
pause
popd
