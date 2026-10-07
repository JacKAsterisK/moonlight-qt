$ErrorActionPreference = 'Stop'

try {
    $Instances = @(Get-Process -Name Moonlight -ErrorAction SilentlyContinue)
    foreach ($Instance in $Instances) {
        Write-Host "Closing Moonlight (PID $($Instance.Id))..."
        if ($Instance.CloseMainWindow()) {
            [void]$Instance.WaitForExit(5000)
        }
        if (-not $Instance.HasExited) {
            Stop-Process -Id $Instance.Id -Force -ErrorAction Stop
            if (-not $Instance.WaitForExit(5000)) {
                throw "Moonlight (PID $($Instance.Id)) did not exit."
            }
        }
    }
    if (Get-Process -Name Moonlight -ErrorAction SilentlyContinue) {
        throw 'A Moonlight instance is still running.'
    }
} catch {
    Write-Error "Cannot update while Moonlight is running: $_"
    exit 1
}
