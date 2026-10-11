param(
    [string]$QtRoot = '',
    [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $QtRoot) { $QtRoot = Join-Path $repo '.tools\Qt\6.12.0\msvc2022_64' }
foreach ($required in @($VcVars, (Join-Path $QtRoot 'bin\qmake.exe'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing WinTab test tool: $required" }
}
$testRoot = Join-Path $repo ('.tools\wintab-mapping-tests\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$batch = @"
@echo off
setlocal
call "$VcVars" >nul
if errorlevel 1 exit /b 1
cd /d "$testRoot"
"$QtRoot\bin\qmake.exe" "$PSScriptRoot\wintabmapping.pro" CONFIG+=release
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
release\test_wintabmapping.exe
exit /b %ERRORLEVEL%
"@
$batchPath = Join-Path $testRoot 'run-tests.bat'
[System.IO.File]::WriteAllText($batchPath, $batch.Replace("`n", "`r`n"), [System.Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "WinTab mapping tests failed (exit $LASTEXITCODE)." }
Write-Host 'WinTab mapping tests use numeric fixtures and do not access a tablet, driver or stream.'
