$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression,System.IO.Compression.FileSystem

function Assert-MoonlightPlainPath {
    <# .SYNOPSIS
    Rejects filesystem links in the installation or staging path.
    #>
    param([Parameter(Mandatory = $true)][string]$Path)
    for ($directory = [IO.DirectoryInfo]::new([IO.Path]::GetFullPath($Path)); $null -ne $directory; $directory = $directory.Parent) {
        if ($directory.Exists -and ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Moonlight installation paths must not pass through filesystem links.'
        }
    }
}

function Read-MoonlightInstallerManifest {
    <# .SYNOPSIS
    Checks the x64 build receipt and payload hash before installation changes.
    #>
    param([Parameter(Mandatory = $true)][string]$PayloadPath)
    $payload = Get-Item -LiteralPath $PayloadPath
    $manifestPath = Join-Path $payload.DirectoryName 'Moonlight.install.json'
    if ($payload.Name -cne 'Moonlight.zip' -or $payload.Length -le 0 -or $payload.Length -gt 1GB -or
        (Get-Item -LiteralPath $manifestPath).Length -gt 16384) { throw 'Invalid Moonlight payload or build receipt size.' }
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or $manifest.architecture -cne 'x64' -or
        $manifest.installer_name -cne 'Moonlight.zip' -or $manifest.source_dirty -isnot [bool] -or
        $manifest.source_commit -cnotmatch '^[0-9a-f]{40}$' -or
        $manifest.installer_sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'Invalid Moonlight build receipt. Recopy the complete package.'
    }
    if ((Get-FileHash -LiteralPath $payload.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.installer_sha256) {
        throw 'Moonlight payload hash does not match its build receipt.'
    }
    return $manifest
}

function Expand-MoonlightPayload {
    <# .SYNOPSIS
    Validates ZIP paths, links, duplicate names and size before extracting into a fresh folder.
    #>
    param([Parameter(Mandatory = $true)][string]$PayloadPath,
        [Parameter(Mandatory = $true)][string]$Destination)
    Assert-MoonlightPlainPath $Destination
    if (Test-Path -LiteralPath $Destination) { throw 'Moonlight staging folder already exists.' }
    $zip = [IO.Compression.ZipFile]::OpenRead($PayloadPath)
    try {
        if ($zip.Entries.Count -eq 0 -or $zip.Entries.Count -gt 10000) { throw 'Invalid Moonlight payload entry count.' }
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $files = [Collections.Generic.List[object]]::new()
        [long]$total = 0
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if ($name.Contains('\') -or $name.StartsWith('/') -or $name.Contains(':') -or $name.Contains([char]0)) {
                throw 'Moonlight payload contains an unsafe path.'
            }
            $directory = $name.EndsWith('/')
            $relative = $name.TrimEnd('/')
            foreach ($part in $relative.Split('/')) {
                if (-not $part -or $part -in @('.', '..') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
                    $part -match '[<>"|?*\x00-\x1f]' -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') {
                    throw 'Moonlight payload contains an unsafe Windows path.'
                }
            }
            $attributes = [uint32]([int64]$entry.ExternalAttributes -band 0xffffffffL)
            $unixType = ($attributes -shr 16) -band 0xf000
            if (($attributes -band 0x400) -ne 0 -or $unixType -notin @(0, 0x4000, 0x8000)) {
                throw 'Moonlight payload contains a filesystem link or special file.'
            }
            if (-not $names.Add($relative)) { throw 'Moonlight payload contains duplicate paths.' }
            if ($directory) { continue }
            if ($relative -ieq 'portable.dat') { throw 'Managed Moonlight packages must use the normal user profile.' }
            $total += $entry.Length
            if ($total -gt 1GB) { throw 'Unpacked Moonlight payload exceeds 1 GiB.' }
            $files.Add($entry)
        }
        foreach ($required in @('Moonlight.exe', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'SDL3.dll', 'vcruntime140.dll')) {
            if (-not $names.Contains($required)) { throw "Moonlight payload is missing $required." }
        }
        $null = New-Item -ItemType Directory -Path $Destination
        foreach ($entry in $files) {
            $target = Join-Path $Destination $entry.FullName.Replace('/', [IO.Path]::DirectorySeparatorChar)
            $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force
            $input = $entry.Open()
            try {
                $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $input.CopyTo($output) } finally { $output.Dispose() }
            } finally { $input.Dispose() }
        }
        Assert-MoonlightX64Executable (Join-Path $Destination 'Moonlight.exe')
    } finally { $zip.Dispose() }
}

function Assert-MoonlightX64Executable {
    <# .SYNOPSIS
    Confirms the packaged application is an AMD64 Windows PE executable.
    #>
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Moonlight.exe is not a Windows executable.' }
        $stream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        if ($offset -gt $stream.Length - 6) { throw 'Invalid Moonlight.exe PE header.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Moonlight.exe must be an x64 Windows executable.' }
    } finally { $reader.Dispose() }
}

function Stop-MoonlightForInstall {
    <# .SYNOPSIS
    Closes this Windows user's Moonlight instances before activating the new binaries.
    #>
    $instances = @(Get-Process -Name Moonlight -ErrorAction SilentlyContinue)
    $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $owned = [Collections.Generic.List[object]]::new()
    foreach ($instance in $instances) {
        $record = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$($instance.Id)" -ErrorAction Stop
        if (-not $record) { continue }
        $owner = Invoke-CimMethod -InputObject $record -MethodName GetOwnerSid -ErrorAction Stop
        if ($owner.ReturnValue -ne 0) { throw 'Could not verify a running Moonlight instance owner.' }
        if ($owner.Sid -cne $currentSid) { continue }
        $owned.Add($instance)
        Write-Information "Closing Moonlight (PID $($instance.Id))..." -InformationAction Continue
        if ($instance.CloseMainWindow()) { $null = $instance.WaitForExit(5000) }
        if (-not $instance.HasExited) {
            Stop-Process -Id $instance.Id -Force -ErrorAction Stop
            if (-not $instance.WaitForExit(5000)) { throw 'A Moonlight instance did not exit.' }
        }
    }
    foreach ($instance in $owned) {
        if (-not $instance.HasExited) { throw 'A Moonlight instance is still running.' }
    }
}

function Invoke-MoonlightPackageInstall {
    <# .SYNOPSIS
    Installs a verified package per user, retaining the previous binaries for recovery.
    .PARAMETER InstallRoot
    Defaults to the current Windows user's LocalAppData MoonlightFork folder.
    #>
    param([Parameter(Mandatory = $true)][string]$PayloadPath,
        [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'MoonlightFork'), [switch]$NoShortcut)
    $manifest = Read-MoonlightInstallerManifest $PayloadPath
    $root = [IO.Path]::GetFullPath($InstallRoot)
    Assert-MoonlightPlainPath $root
    $null = New-Item -ItemType Directory -Path $root -Force
    $lock = [IO.File]::Open((Join-Path $root 'install.lock'), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $current = Join-Path $root 'current'
        Assert-MoonlightPlainPath $current
        $stage = Join-Path $root ('.staging-' + [guid]::NewGuid().ToString('N'))
        Expand-MoonlightPayload $PayloadPath $stage
        Copy-Item -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($PayloadPath))) 'Moonlight.install.json') -Destination $stage
        Stop-MoonlightForInstall
        $backup = Join-Path $root ('backup-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
        $hadCurrent = Test-Path -LiteralPath $current
        if ($hadCurrent) { Move-MoonlightDirectory $current $backup }
        try { Move-MoonlightDirectory $stage $current }
        catch {
            if ($hadCurrent) { Move-MoonlightDirectory $backup $current }
            throw
        }
        if (-not $NoShortcut) {
            try {
                $shell = New-Object -ComObject WScript.Shell
                $shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Programs')) 'Moonlight Fork.lnk'))
                $shortcut.TargetPath = Join-Path $current 'Moonlight.exe'
                $shortcut.WorkingDirectory = $current
                $shortcut.Description = 'Moonlight studio fork'
                $shortcut.Save()
            } catch { Write-Warning 'Moonlight installed, but its Start menu shortcut could not be created.' }
        }
        Write-Information "Installed Moonlight fork $($manifest.source_commit.Substring(0,8)) at $current" -InformationAction Continue
        if ($hadCurrent) { Write-Information "Previous binaries retained at $backup" -InformationAction Continue }
        return Join-Path $current 'Moonlight.exe'
    } finally { $lock.Dispose() }
}

function Move-MoonlightDirectory {
    <# .SYNOPSIS
    Activates or restores a complete binary folder with a same-volume directory rename.
    #>
    param([string]$Source, [string]$Destination)
    [IO.Directory]::Move($Source, $Destination)
}

Export-ModuleMember -Function Assert-MoonlightPlainPath,Read-MoonlightInstallerManifest,Expand-MoonlightPayload,Assert-MoonlightX64Executable,Stop-MoonlightForInstall,Invoke-MoonlightPackageInstall
