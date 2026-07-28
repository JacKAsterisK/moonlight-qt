@echo off
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"

if /I "%~1"=="--help" goto :Help
if /I "%~1"=="/?" goto :Help

set "BUILD_CONFIG=%~1"
if "%BUILD_CONFIG%"=="" set "BUILD_CONFIG=Release"

if /I "%BUILD_CONFIG%"=="Release" (
    set "BUILD_CONFIG=Release"
    set "BUILD_CONFIG_LOWER=release"
) else if /I "%BUILD_CONFIG%"=="Debug" (
    set "BUILD_CONFIG=Debug"
    set "BUILD_CONFIG_LOWER=debug"
) else (
    echo Invalid configuration: %BUILD_CONFIG%
    echo Expected Release or Debug.
    exit /b 2
)

set "QT_VERSION=6.11.1"
if defined ML_QT_VERSION set "QT_VERSION=%ML_QT_VERSION%"

set "QT_ROOT=%CD%\.tools\Qt"
if defined ML_QT_ROOT set "QT_ROOT=%ML_QT_ROOT%"
set "QT_BIN=%QT_ROOT%\%QT_VERSION%\msvc2022_64\bin"
set "DEPLOY_DIR=%CD%\build\deploy-x64-%BUILD_CONFIG_LOWER%"
set "BIN_DIR=%CD%\bin"

if not "%CD%"=="%CD: =%" (
    echo Moonlight's upstream Windows build script does not safely handle spaces.
    echo Move the repository to a path without spaces and try again.
    exit /b 1
)

if not exist "%QT_BIN%\qmake.exe" (
    echo Qt %QT_VERSION% was not found at:
    echo   %QT_BIN%
    echo Run setup.bat first.
    exit /b 1
)

if not exist "libs\windows\lib\x64\SDL3.dll" (
    echo Moonlight's Windows dependencies are missing.
    echo Run setup.bat first.
    exit /b 1
)

if not exist "app\SDL_GameControllerDB\gamecontrollerdb.txt" (
    echo Git submodules are missing.
    echo Run setup.bat first.
    exit /b 1
)

set "PATH=%QT_BIN%;%ProgramFiles%\7-Zip;%ProgramFiles(x86)%\7-Zip;%PATH%"
where 7z.exe >nul 2>&1
if errorlevel 1 (
    echo 7-Zip was not found. Run setup.bat first.
    exit /b 1
)

if not defined CI_VERSION (
    for /f "delims=" %%I in ('git rev-parse --short=7 HEAD') do set "CI_VERSION=%%I"
)

echo.
echo Building Moonlight %BUILD_CONFIG% for x64...
echo Qt:     %QT_BIN%
echo Output: %BIN_DIR%
echo.

call "%CD%\scripts\build-arch.bat" "%BUILD_CONFIG%" x64
if errorlevel 1 (
    echo Moonlight build failed.
    exit /b 1
)

if not exist "%DEPLOY_DIR%\Moonlight.exe" (
    echo Build completed without producing %DEPLOY_DIR%\Moonlight.exe
    exit /b 1
)

echo Refreshing bin directory...
robocopy "%DEPLOY_DIR%" "%BIN_DIR%" /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
set "ROBOCOPY_RESULT=!ERRORLEVEL!"
if !ROBOCOPY_RESULT! GEQ 8 (
    echo Failed to copy the build into %BIN_DIR%
    echo Close Moonlight if it is currently running from that directory.
    exit /b !ROBOCOPY_RESULT!
)

echo.
echo Build complete:
echo   %BIN_DIR%\Moonlight.exe
echo.
echo This build shares the normal Moonlight profile and pairings.
echo Rename bin\portable.dat.inactive to portable.dat for an isolated profile.
exit /b 0

:Help
echo Usage:
echo   build.bat          Build a Release x64 binary
echo   build.bat Release  Build a Release x64 binary
echo   build.bat Debug    Build a Debug x64 binary
echo.
echo Output:
echo   bin\Moonlight.exe
echo.
echo Run setup.bat once on each machine before building.
exit /b 0
