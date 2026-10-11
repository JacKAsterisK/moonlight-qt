<# .SYNOPSIS
Builds a self-contained x64 Moonlight package or installs an already built package.
#>
[CmdletBinding()]
param([ValidateSet('Package', 'Install', 'Verify', 'Smoke')][string]$Action = 'Install',
    [string]$PayloadPath, [string]$InstallRoot, [switch]$Help)
$ErrorActionPreference = 'Stop'
$InformationPreference = 'Continue'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $PSScriptRoot 'MoonlightInstall.psm1') -Force

function New-MoonlightZip {
    <# .SYNOPSIS
    Creates forward-slash ZIP paths consistently on Windows PowerShell 5.1 and PowerShell 7.
    #>
    param([string]$Directory, [string]$Path)
    Assert-MoonlightPlainPath $Directory
    $prefix = [IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    $output = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $zip = [IO.Compression.ZipArchive]::new($output, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName) {
            Assert-MoonlightPlainPath $file.DirectoryName
            if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Build output contains a filesystem link.' }
            $name = $file.FullName.Substring($prefix.Length).Replace('\', '/')
            $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $input = [IO.File]::OpenRead($file.FullName)
            try {
                $target = $entry.Open()
                try { $input.CopyTo($target) } finally { $target.Dispose() }
            } finally { $input.Dispose() }
        }
    } finally { $zip.Dispose(); $output.Dispose() }
}

function Invoke-MoonlightPackageBuild {
    <# .SYNOPSIS
    Builds the current checkout before recording its source identity and payload hash.
    #>
    Set-Location -LiteralPath $repo
    $commit = & git.exe rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $commit -cnotmatch '^[0-9a-f]{40}$') { throw 'Could not read the source commit.' }
    $dirty = & git.exe status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0) { throw 'Could not read the source state.' }
    $env:ML_BUILD_INNER = '1'
    $env:ML_BUILD_NO_BIN = '1'
    $env:ML_BUILD_PORTABLE_ONLY = '1'
    & (Join-Path $repo 'build.bat') Release
    if ($LASTEXITCODE -ne 0) { throw 'Moonlight build failed.' }
    $afterCommit = & git.exe rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $afterCommit -cne $commit) { throw 'Source commit changed during the build.' }
    $afterDirty = & git.exe status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0 -or ($dirty -join "`n") -cne ($afterDirty -join "`n")) { throw 'Source state changed during the build.' }
    $output = Join-Path $repo 'build\package-x64-release'
    Assert-MoonlightPlainPath $output
    $null = New-Item -ItemType Directory -Path $output -Force
    $bundle = Join-Path $output ('bundle-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path (Join-Path $bundle 'scripts\windows') -Force
    $payload = Join-Path $bundle 'Moonlight.zip'
    New-MoonlightZip (Join-Path $repo 'build\deploy-x64-release') $payload
    $receipt = [ordered]@{
        schema_version = 1; architecture = 'x64'; installer_name = 'Moonlight.zip'
        installer_sha256 = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
        source_commit = $commit.Trim(); source_dirty = [bool]$dirty; built_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    $receiptPath = Join-Path $bundle 'Moonlight.install.json'
    [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Copy-Item -LiteralPath (Join-Path $repo 'install.bat') -Destination $bundle
    Copy-Item -LiteralPath (Join-Path $repo 'docs\windows-fork-install.md') -Destination (Join-Path $bundle 'README.md')
    Copy-Item -LiteralPath (Join-Path $repo 'scripts\windows\moonlight-windows.ps1'),(Join-Path $repo 'scripts\windows\MoonlightInstall.psm1') -Destination (Join-Path $bundle 'scripts\windows')
    $null = Read-MoonlightInstallerManifest $payload
    $check = Join-Path $output ('.verify-' + [guid]::NewGuid().ToString('N'))
    Expand-MoonlightPayload $payload $check
    Invoke-MoonlightSmoke (Join-Path $check 'Moonlight.exe')
    $archive = Join-Path $output 'Moonlight-fork-windows.zip'
    $temporaryArchive = Join-Path $output ('.package-' + [guid]::NewGuid().ToString('N') + '.zip')
    New-MoonlightZip $bundle $temporaryArchive
    Copy-Item -LiteralPath $receiptPath -Destination (Join-Path $output 'Moonlight.install.json') -Force
    Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
    Write-Information "Deployment bundle: $archive"
}

function Invoke-MoonlightSmoke {
    <# .SYNOPSIS
    Checks the packaged executable without Qt or compiler directories in PATH.
    #>
    param([string]$Executable)
    # The executable initializes its profile before parsing --version. A marker
    # in this fresh verification folder keeps that initialization isolated.
    $profileMarker = Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'portable.dat'
    $marker = [IO.File]::Open($profileMarker, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $marker.Dispose()
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = [IO.Path]::GetDirectoryName($Executable)
    $start.Arguments = '--version'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Moonlight version smoke test timed out.' }
        $text = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $text -notmatch 'Moonlight.*\d+\.\d+') { throw 'Packaged Moonlight failed its version smoke test.' }
        Write-Information $text.Trim()
    } finally { $process.Dispose(); Remove-Item -LiteralPath $profileMarker -Force }
}

try {
    if ($Help) {
        Write-Information 'package.bat                 Build this checkout and create a deployable x64 bundle'
        Write-Information 'install.bat                 Install an adjacent bundle without build tools or administrator rights'
        Write-Information 'install.bat -PayloadPath C:\path\Moonlight.zip   Install a build with its adjacent receipt'
        Write-Information 'Use -Action Verify or -Action Smoke with -PayloadPath to check a copied payload.'
        Write-Information 'MOONLIGHT_NO_PAUSE=1 disables the batch pause. ML_BUILD_JOBS limits jom parallelism.'
        exit 0
    }
    if ($Action -eq 'Package') {
        if ($PayloadPath -or $InstallRoot) { throw 'Package does not accept installation paths.' }
        Invoke-MoonlightPackageBuild
        exit 0
    }
    if (-not $PayloadPath) { $PayloadPath = Join-Path $repo 'Moonlight.zip' }
    $PayloadPath = [IO.Path]::GetFullPath($PayloadPath)
    $null = Read-MoonlightInstallerManifest $PayloadPath
    if ($Action -eq 'Install') {
        $parameters = @{ PayloadPath = $PayloadPath }
        if ($InstallRoot) { $parameters.InstallRoot = $InstallRoot }
        $null = Invoke-MoonlightPackageInstall @parameters
        exit 0
    }
    $stage = Join-Path ([IO.Path]::GetTempPath()) ('moonlight-verify-' + [guid]::NewGuid().ToString('N'))
    Expand-MoonlightPayload $PayloadPath $stage
    if ($Action -eq 'Smoke') { Invoke-MoonlightSmoke (Join-Path $stage 'Moonlight.exe') }
    Write-Information 'Moonlight payload verified.'
    exit 0
} catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
