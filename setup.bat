@echo off
if defined ML_SETUP_INNER goto :SetupMain
setlocal
set "ML_SETUP_INNER=1"
call "%~f0" %*
set "ML_SETUP_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %ML_SETUP_RESULT%

:SetupMain
setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0"
set "CHECK_ONLY="
set "INSTALLED_QT_VERSION="

if /I "%~1"=="--help" goto :Help
if /I "%~1"=="/?" goto :Help
if /I "%~1"=="--check" (
    set "CHECK_ONLY=1"
) else if not "%~1"=="" (
    echo Unknown option: %~1
    echo Run setup.bat --help for usage.
    exit /b 2
)

set "QT_VERSION=6.12.0"
if defined ML_QT_VERSION set "QT_VERSION=%ML_QT_VERSION%"

set "TOOLS_DIR=%CD%\.tools"
set "QT_ROOT=%TOOLS_DIR%\Qt"
if defined ML_QT_ROOT set "QT_ROOT=%ML_QT_ROOT%"
set "QT_BIN=%QT_ROOT%\%QT_VERSION%\msvc2022_64\bin"
set "AQTVENV=%TOOLS_DIR%\aqt-venv"
set "AQT_COMMIT=073e34d7c2ab4ae6961ed7cca690b3abd5ba5a7e"
set "MISSING=0"

echo.
echo Moonlight Windows build setup
echo Repository: %CD%
echo Qt:         %QT_VERSION%
echo.

echo [1/5] Checking Visual Studio C++ tools...
call :FindVisualStudio
if defined VS_PATH (
    echo Found Visual Studio: !VS_PATH!
) else (
    if defined CHECK_ONLY (
        echo MISSING: Visual Studio 2022 C++ build tools
        set "MISSING=1"
    ) else (
        call :RequireWinget || exit /b 1
        echo Installing Visual Studio 2022 Build Tools...
        winget install --exact --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-package-agreements --accept-source-agreements --override "--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        if errorlevel 1 (
            echo Failed to install Visual Studio Build Tools.
            exit /b 1
        )
        call :FindVisualStudio
        if not defined VS_PATH (
            echo Visual Studio was installed, but it is not visible yet.
            echo Restart Windows or open a new terminal, then run setup.bat again.
            exit /b 1
        )
    )
)

echo [2/5] Checking 7-Zip...
call :FindSevenZip
if defined SEVENZIP_DIR (
    echo Found 7-Zip: !SEVENZIP_DIR!
) else (
    if defined CHECK_ONLY (
        echo MISSING: 7-Zip
        set "MISSING=1"
    ) else (
        call :RequireWinget || exit /b 1
        echo Installing 7-Zip...
        winget install --exact --id 7zip.7zip --source winget --accept-package-agreements --accept-source-agreements
        if errorlevel 1 (
            echo Failed to install 7-Zip.
            exit /b 1
        )
        call :FindSevenZip
        if not defined SEVENZIP_DIR (
            echo 7-Zip was installed, but it is not visible yet.
            echo Open a new terminal and run setup.bat again.
            exit /b 1
        )
    )
)

echo [3/5] Checking Python and Qt...
if exist "%QT_BIN%\qmake.exe" (
    for /f "usebackq delims=" %%V in (`"%QT_BIN%\qmake.exe" -query QT_VERSION`) do set "INSTALLED_QT_VERSION=%%V"
)

if /I "%INSTALLED_QT_VERSION%"=="%QT_VERSION%" (
    echo Found Qt %INSTALLED_QT_VERSION%: %QT_BIN%
) else (
    if defined CHECK_ONLY (
        echo MISSING: Qt %QT_VERSION% MSVC 2022 x64 at %QT_BIN%
        set "MISSING=1"
    ) else (
        call :FindPython
        if not defined PYTHON_CMD (
            call :RequireWinget || exit /b 1
            echo Installing Python 3.12...
            winget install --exact --id Python.Python.3.12 --source winget --accept-package-agreements --accept-source-agreements
            if errorlevel 1 (
                echo Failed to install Python.
                exit /b 1
            )
            call :FindPython
        )
        if not defined PYTHON_CMD (
            echo Python was installed, but it is not visible yet.
            echo Open a new terminal and run setup.bat again.
            exit /b 1
        )

        if not exist "%AQTVENV%\Scripts\python.exe" (
            echo Creating local Python environment...
            if not exist "%TOOLS_DIR%" mkdir "%TOOLS_DIR%"
            !PYTHON_CMD! -m venv "%AQTVENV%"
            if errorlevel 1 exit /b 1
        )

        echo Installing the Qt downloader used by Moonlight CI...
        "%AQTVENV%\Scripts\python.exe" -m pip install --disable-pip-version-check --upgrade "git+https://github.com/miurahr/aqtinstall.git@%AQT_COMMIT%"
        if errorlevel 1 exit /b 1

        echo Installing Qt %QT_VERSION% MSVC 2022 x64...
        "%AQTVENV%\Scripts\python.exe" -m aqt install-qt windows desktop "%QT_VERSION%" win64_msvc2022_64 --outputdir "%QT_ROOT%"
        if errorlevel 1 exit /b 1

        if not exist "%QT_BIN%\qmake.exe" (
            echo Qt installation completed without producing %QT_BIN%\qmake.exe
            exit /b 1
        )
    )
)

echo [4/5] Checking Git submodules...
if defined CHECK_ONLY (
    if exist "app\SDL_GameControllerDB\gamecontrollerdb.txt" (
        echo Git submodules are initialized.
    ) else (
        echo MISSING: Git submodules
        set "MISSING=1"
    )
) else (
    git submodule update --init --recursive
    if errorlevel 1 exit /b 1
)

echo [5/5] Checking Moonlight prebuilt dependencies...
if defined CHECK_ONLY (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CD%\setup-deps.ps1" -Check
    if errorlevel 1 set "MISSING=1"
) else (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CD%\setup-deps.ps1"
    if errorlevel 1 exit /b 1
)

echo.
if defined CHECK_ONLY (
    if "%MISSING%"=="0" (
        echo Setup check passed. Run build.bat to compile Moonlight.
        exit /b 0
    )
    echo Setup check found missing requirements. Run setup.bat to install them.
    exit /b 1
)

echo Setup complete. Run build.bat to compile Moonlight into:
echo   %CD%\bin\Moonlight.exe
exit /b 0

:FindVisualStudio
set "VS_PATH="
call "%CD%\scripts\find-vswhere.bat"
if errorlevel 1 exit /b 0
for /f "usebackq delims=" %%I in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%I"
exit /b 0

:FindSevenZip
set "SEVENZIP_DIR="
where 7z.exe >nul 2>&1
if not errorlevel 1 (
    for /f "delims=" %%I in ('where 7z.exe 2^>nul') do if not defined SEVENZIP_DIR set "SEVENZIP_DIR=%%~dpI"
)
if not defined SEVENZIP_DIR if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP_DIR=%ProgramFiles%\7-Zip"
if not defined SEVENZIP_DIR if exist "%ProgramFiles(x86)%\7-Zip\7z.exe" set "SEVENZIP_DIR=%ProgramFiles(x86)%\7-Zip"
exit /b 0

:FindPython
set "PYTHON_CMD="
py.exe -3 -c "import sys" >nul 2>&1
if not errorlevel 1 set "PYTHON_CMD=py.exe -3"
if defined PYTHON_CMD exit /b 0

for /f "delims=" %%I in ('where python.exe 2^>nul') do (
    if not defined PYTHON_CMD (
        "%%I" -c "import sys" >nul 2>&1
        if not errorlevel 1 set PYTHON_CMD="%%I"
    )
)
if defined PYTHON_CMD exit /b 0

for /f "delims=" %%I in ('dir /b /s /a-d "%LocalAppData%\Programs\Python\Python3*\python.exe" 2^>nul') do (
    if not defined PYTHON_CMD set PYTHON_CMD="%%I"
)
exit /b 0

:RequireWinget
where winget.exe >nul 2>&1
if errorlevel 1 (
    echo This machine is missing a required build tool and Windows Package Manager.
    echo Install App Installer from Microsoft, then run setup.bat again.
    exit /b 1
)
exit /b 0

:Help
echo Usage:
echo   setup.bat          Install/check build tools, Qt, submodules, and dependencies
echo   setup.bat --check  Check requirements without changing the machine
echo.
echo Optional environment overrides:
echo   ML_QT_VERSION      Qt version to install ^(default: 6.12.0^)
echo   ML_QT_ROOT         Qt installation root ^(default: .tools\Qt^)
exit /b 0
