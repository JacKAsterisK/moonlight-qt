@echo off
if defined ML_UPDATE_INNER goto :UpdateMain
setlocal
set "ML_UPDATE_INNER=1"
call "%~f0" %*
set "ML_UPDATE_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %ML_UPDATE_RESULT%

:UpdateMain
setlocal EnableExtensions
cd /d "%~dp0"

if /I "%~1"=="--help" goto :Help
if /I "%~1"=="/?" goto :Help
set "SKIP_PULL="
if /I "%~1"=="--no-pull" (
    set "SKIP_PULL=1"
) else if not "%~1"=="" (
    echo Unknown option: %~1
    exit /b 2
)
if not "%~2"=="" (
    echo Too many arguments. Run update.bat --help for usage.
    exit /b 2
)

echo Updating Moonlight in %CD%
if not defined SKIP_PULL (
    echo [1/4] Pulling the current branch from its configured remote...
    git pull --ff-only
    if errorlevel 1 (
        echo Source update failed. Resolve local changes or branch divergence, then retry.
        exit /b 1
    )
)

echo [2/4] Refreshing required build tools and dependencies...
set "ML_SETUP_INNER=1"
call "%CD%\setup.bat"
if errorlevel 1 exit /b 1

echo [3/4] Closing running Moonlight instances...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\stop-moonlight.ps1"
if errorlevel 1 exit /b 1

echo [4/4] Building and updating bin\Moonlight.exe...
set "ML_BUILD_INNER=1"
call "%CD%\build.bat" Release
if errorlevel 1 exit /b 1

echo.
echo Update complete: %CD%\bin\Moonlight.exe
exit /b 0

:Help
echo Usage:
echo   update.bat            Pull this fork, refresh dependencies, close Moonlight, and rebuild
echo   update.bat --no-pull  Rebuild the current checkout without pulling
echo.
echo Updates the fork's binary in bin; closes all running Moonlight instances.
echo Existing settings and pairings are preserved.
echo ML_QT_VERSION and ML_QT_ROOT overrides are also supported.
exit /b 0
