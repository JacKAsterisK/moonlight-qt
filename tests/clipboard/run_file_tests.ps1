param([string]$VcVars = '')
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio locator is unavailable.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'A Visual C++ x64 toolchain is required.' }
    $VcVars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $VcVars -PathType Leaf)) { throw 'Missing Visual C++ environment script.' }
$testRoot = Join-Path $repo ('.tools\clipboard-files-build\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$batch = @"
@echo off
setlocal
call "$VcVars" >nul
if errorlevel 1 exit /b 1
set "PATH=$repo\libs\windows\lib\x64;%PATH%"
cd /d "$testRoot"
cl /nologo /std:c++17 /EHsc /MD /utf-8 /O2 /I"$repo\libs\windows\include\x64" "$PSScriptRoot\test_clipboardfiles.cpp" /Fe:test_clipboardfiles.exe /link /LIBPATH:"$repo\libs\windows\lib\x64" libcrypto.lib user32.lib shell32.lib
if errorlevel 1 exit /b 1
test_clipboardfiles.exe
exit /b %ERRORLEVEL%
"@
$batchPath = Join-Path $testRoot 'run-tests.bat'
[System.IO.File]::WriteAllText($batchPath, $batch.Replace("`n", "`r`n"), [System.Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "Clipboard file tests failed (exit $LASTEXITCODE)." }
Write-Host 'Tests used isolated temporary files only; the real clipboard was never read or changed.'
