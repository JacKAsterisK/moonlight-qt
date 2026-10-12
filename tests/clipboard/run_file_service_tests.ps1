param(
    [Parameter(Mandatory = $true)][string]$ServiceSourceRoot,
    [string]$VcVars = ''
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$ServiceSourceRoot = [System.IO.Path]::GetFullPath($ServiceSourceRoot)
foreach ($source in @('clipboard_files.h', 'clipboard_files.cpp', 'clipboard_files_engine.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ServiceSourceRoot $source) -PathType Leaf)) {
        throw "Missing actual Sunshine service source: $source"
    }
}
$ServiceIncludeRoot = Split-Path -Parent $ServiceSourceRoot
if (-not [string]::Equals((Join-Path $ServiceIncludeRoot 'src'), $ServiceSourceRoot.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ServiceSourceRoot must be the src directory of a Sunshine checkout.'
}
if (-not $VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'A Visual C++ x64 toolchain is required.' }
    $VcVars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $VcVars -PathType Leaf)) { throw 'Missing Visual C++ environment script.' }
$testRoot = Join-Path $repo ('.tools\clipboard-files-service-tests\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$batch = @"
@echo off
setlocal
call "$VcVars" >nul
if errorlevel 1 exit /b 1
set "PATH=$repo\libs\windows\lib\x64;%PATH%"
cd /d "$testRoot"
cl /nologo /std:c++17 /EHsc /MD /utf-8 /O2 /I"$repo\libs\windows\include\x64" /I"$ServiceIncludeRoot" "$PSScriptRoot\test_clipboardfiles_lifecycle.cpp" "$ServiceSourceRoot\clipboard_files.cpp" /Fe:test_clipboardfiles_lifecycle.exe /link /LIBPATH:"$repo\libs\windows\lib\x64" libcrypto.lib user32.lib shell32.lib Wtsapi32.lib userenv.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
test_clipboardfiles_lifecycle.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /MD /utf-8 /O2 /I"$repo\libs\windows\include\x64" /I"$ServiceIncludeRoot" "$PSScriptRoot\test_clipboardfiles_context.cpp" /Fe:test_clipboardfiles_context.exe /link /LIBPATH:"$repo\libs\windows\lib\x64" libcrypto.lib user32.lib shell32.lib Wtsapi32.lib userenv.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
test_clipboardfiles_context.exe
exit /b %ERRORLEVEL%
"@
$batchPath = Join-Path $testRoot 'run-tests.bat'
[System.IO.File]::WriteAllText($batchPath, $batch.Replace("`n", "`r`n"), [System.Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "Clipboard service lifecycle tests failed (exit $LASTEXITCODE)." }
Write-Host 'Actual Sunshine service sources used isolated temporary files and in-memory desktop callbacks only.'
