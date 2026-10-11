@echo off
setlocal EnableExtensions
pushd "%~dp0" || exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\moonlight-windows.ps1" -Action Install %*
set "MOONLIGHT_RESULT=%ERRORLEVEL%"
popd
if not defined MOONLIGHT_NO_PAUSE pause
exit /b %MOONLIGHT_RESULT%
