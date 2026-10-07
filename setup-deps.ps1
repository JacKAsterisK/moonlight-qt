param([switch]$Check, [switch]$Force)

$ErrorActionPreference = 'Stop'

$Organization = "moonlight-stream"
$PrebuiltRepo = "moonlight-qt-deps"
$TargetDir = Join-Path $PSScriptRoot "libs\windows"
$Assets = @("windows-x64.zip", "windows-ARM64.zip")
$Tag = "v19"
$VersionFile = Join-Path $TargetDir '.moonlight-deps-version'
$RequiredFiles = @('lib\x64\SDL3.dll', 'lib\x64\avcodec.lib', 'lib\ARM64\SDL3.dll', 'lib\ARM64\avcodec.lib')
$Current = (Test-Path -LiteralPath $VersionFile) -and ((Get-Content -LiteralPath $VersionFile -Raw).Trim() -eq $Tag)
foreach ($RequiredFile in $RequiredFiles) {
    $Current = $Current -and (Test-Path -LiteralPath (Join-Path $TargetDir $RequiredFile))
}

if ($Check) {
    if ($Current) {
        Write-Host "Moonlight dependencies $Tag are current."
        exit 0
    }
    Write-Host "MISSING or OUTDATED: Moonlight dependencies $Tag. Run setup.bat."
    exit 1
}
if ($Current -and -not $Force) {
    Write-Host "Moonlight dependencies $Tag are already installed."
    exit 0
}

# Stage both architectures before replacing the installed dependency bundle.
$LibsDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'libs'))
$StageDir = Join-Path $LibsDir ('.windows-next-' + [guid]::NewGuid().ToString('N'))
$BackupDir = Join-Path $LibsDir ('.windows-previous-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
try {
    foreach ($AssetName in $Assets) {
        $Url = "https://github.com/$Organization/$PrebuiltRepo/releases/download/$Tag/$AssetName"
        $ArchivePath = Join-Path $StageDir $AssetName
        Write-Host "Downloading $AssetName..." -ForegroundColor Cyan
        curl.exe --silent --show-error --location --fail --retry 3 --output "$ArchivePath" "$Url"
        if ($LASTEXITCODE -ne 0) {
            throw "Downloading $AssetName failed (exit $LASTEXITCODE). Existing dependencies were preserved."
        }
        Write-Host "Extracting $AssetName..." -ForegroundColor Cyan
        Expand-Archive -LiteralPath $ArchivePath -DestinationPath $StageDir -Force
        Remove-Item -LiteralPath $ArchivePath
    }
    foreach ($RequiredFile in $RequiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $StageDir $RequiredFile))) {
            throw "Dependency bundle is missing $RequiredFile."
        }
    }
    Set-Content -LiteralPath (Join-Path $StageDir '.moonlight-deps-version') -Value $Tag -Encoding ASCII
    if (Test-Path -LiteralPath $TargetDir) {
        Move-Item -LiteralPath $TargetDir -Destination $BackupDir
    }
    try {
        Move-Item -LiteralPath $StageDir -Destination $TargetDir
    } catch {
        if (Test-Path -LiteralPath $BackupDir) {
            Move-Item -LiteralPath $BackupDir -Destination $TargetDir
        }
        throw
    }
    if (Test-Path -LiteralPath $BackupDir) {
        # BackupDir is the generated direct child of the repository's libs directory.
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($BackupDir)) -ne $LibsDir) {
            throw 'Unexpected dependency backup path.'
        }
        Remove-Item -LiteralPath $BackupDir -Recurse -Force
    }
} finally {
    if (Test-Path -LiteralPath $StageDir) {
        # StageDir is the generated direct child of the repository's libs directory.
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($StageDir)) -ne $LibsDir) {
            throw 'Unexpected dependency staging path.'
        }
        Remove-Item -LiteralPath $StageDir -Recurse -Force
    }
}

Write-Host "Dependencies successfully deployed" -ForegroundColor Green
