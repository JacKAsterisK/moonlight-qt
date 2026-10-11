<# .SYNOPSIS
Checks package rejection and installation recovery in isolated temporary folders.
#>
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\..\scripts\windows\MoonlightInstall.psm1') -Force
$module = Get-Module MoonlightInstall
$root = Join-Path ([IO.Path]::GetTempPath()) ('moonlight-install-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$count = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Assert-Rejected([scriptblock]$Operation, [string]$Message) {
    $rejected = $false
    try { & $Operation | Out-Null } catch { $rejected = $true }
    Assert-True $rejected $Message
}
function New-Payload([string]$Name, [string]$ExtraName = '', [int]$ExtraAttributes = 0) {
    $folder = Join-Path $root $Name
    $null = New-Item -ItemType Directory -Path $folder
    $path = Join-Path $folder 'Moonlight.zip'
    $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in @('Moonlight.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Network.dll','SDL3.dll','vcruntime140.dll')) {
            $entry = $zip.CreateEntry($name)
            $output = $entry.Open()
            try {
                if ($name -eq 'Moonlight.exe') {
                    $bytes = [byte[]]::new(128)
                    $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[0x3c] = 0x40
                    $bytes[0x40] = 0x50; $bytes[0x41] = 0x45; $bytes[0x44] = 0x64; $bytes[0x45] = 0x86
                    $output.Write($bytes, 0, $bytes.Length)
                } else { $output.WriteByte(1) }
            } finally { $output.Dispose() }
        }
        if ($ExtraName) {
            $entry = $zip.CreateEntry($ExtraName)
            $entry.ExternalAttributes = $ExtraAttributes
            $output = $entry.Open()
            try { $output.WriteByte(2) } finally { $output.Dispose() }
        }
    } finally { $zip.Dispose(); $stream.Dispose() }
    $receipt = @{schema_version=1;architecture='x64';installer_name='Moonlight.zip';installer_sha256=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant();source_commit=('a' * 40);source_dirty=$false;built_utc=[DateTimeOffset]::UtcNow.ToString('o')}
    [IO.File]::WriteAllText((Join-Path $folder 'Moonlight.install.json'), ($receipt | ConvertTo-Json))
    return $path
}

# The isolated installer test never closes a real Moonlight process.
& $module {
    $script:stopCount = 0
    function script:Stop-MoonlightForInstall { $script:stopCount++ }
    function script:Move-MoonlightDirectory {
        param([string]$Source, [string]$Destination)
        if ($script:failActivation -and [IO.Path]::GetFileName($Source).StartsWith('.staging-')) {
            throw 'Injected activation failure.'
        }
        [IO.Directory]::Move($Source, $Destination)
    }
    $script:failActivation = $false
}

try {
    $valid = New-Payload 'valid'
    $null = Read-MoonlightInstallerManifest $valid
    $count++
    foreach ($case in @(
        @{Name='traversal';Path='../outside.txt';Attributes=0},
        @{Name='backslash';Path='sub\outside.txt';Attributes=0},
        @{Name='duplicate';Path='moonlight.EXE';Attributes=0},
        @{Name='reserved';Path='qml/CON.txt';Attributes=0},
        @{Name='profile';Path='portable.dat';Attributes=0},
        @{Name='link';Path='link';Attributes=-1610612736}
    )) {
        $payload = New-Payload $case.Name $case.Path $case.Attributes
        $destination = Join-Path $root ($case.Name + '-stage')
        Assert-Rejected { Expand-MoonlightPayload $payload $destination } "Accepted unsafe payload: $($case.Name)"
        Assert-True (-not (Test-Path -LiteralPath $destination)) 'An invalid payload created a staging folder.'
        $count++
    }
    $tampered = New-Payload 'tampered'
    [IO.File]::AppendAllText($tampered, 'changed')
    Assert-Rejected { Read-MoonlightInstallerManifest $tampered } 'Accepted a mismatched payload hash.'
    $count++
    $junctionTarget = Join-Path $root 'junction-target'
    $junctionRoot = Join-Path $root 'junction-root'
    $null = New-Item -ItemType Directory -Path $junctionTarget
    $null = New-Item -ItemType Junction -Path $junctionRoot -Target $junctionTarget
    Assert-Rejected { Invoke-MoonlightPackageInstall $valid -InstallRoot $junctionRoot -NoShortcut } 'Accepted an installation through a filesystem link.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $junctionTarget 'install.lock'))) 'Linked installation root was changed before rejection.'
    $count++
    $installRoot = Join-Path $root 'installation'
    $installed = Invoke-MoonlightPackageInstall $valid -InstallRoot $installRoot -NoShortcut
    Assert-True (Test-Path -LiteralPath $installed) 'Initial installation did not activate Moonlight.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $installRoot 'current\portable.dat'))) 'Installation enabled a replaceable portable profile.'
    $count++
    [IO.File]::WriteAllText((Join-Path $installRoot 'current\previous-build.txt'), 'previous')
    & $module { $script:failActivation = $true }
    Assert-Rejected { Invoke-MoonlightPackageInstall $valid -InstallRoot $installRoot -NoShortcut } 'Activation failure was not reported.'
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'current\previous-build.txt')) 'Activation failure did not restore the previous binaries.'
    Assert-True (Test-Path -LiteralPath (Join-Path $installRoot 'current\Moonlight.exe')) 'Restored installation lost its executable.'
    $count++
    & $module { $script:failActivation = $false }
    $null = Invoke-MoonlightPackageInstall $valid -InstallRoot $installRoot -NoShortcut
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $installRoot 'current\previous-build.txt'))) 'Successful update retained obsolete files in current.'
    $backups = @(Get-ChildItem -LiteralPath $installRoot -Directory -Filter 'backup-*')
    Assert-True ($backups.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $backups[0].FullName 'previous-build.txt'))) 'Successful update did not retain its recovery folder.'
    $count++
    Write-Host "$count Moonlight package/install checks passed. No live Moonlight processes or profiles were changed."
} catch {
    Write-Host $_.Exception.ToString()
    throw
} finally {
    # Only delete this test's resolved, fresh temporary directory.
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolvedRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolvedRoot).StartsWith('moonlight-install-tests-')) { throw 'Unsafe test cleanup path.' }
    try { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
    catch { Write-Warning "Test files retained at $resolvedRoot" }
}
