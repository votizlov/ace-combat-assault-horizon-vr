param(
    [string]$SteamVR = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$gameFolder = [IO.Path]::GetFullPath((Join-Path $source '..'))
$package = Join-Path $source 'package'
$hostExe = Join-Path $package 'acah_vr_host.exe'
$gameExe = Join-Path $gameFolder 'Ace Combat_AH.exe'
$hookDll = Join-Path $gameFolder 'd3d9.dll'
$configPath = Join-Path $gameFolder 'acah_vr.ini'
$runtime = Join-Path $SteamVR 'steamxr_win64.json'

function Get-LauncherHash([string]$Path) {
    # Use .NET directly: Windows PowerShell may inherit a PowerShell 7 module path.
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

foreach ($file in @($gameExe, $hostExe, (Join-Path $package 'openxr_loader.dll'), $hookDll, $configPath, $runtime)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file is missing: $file" }
}
$packagedHook = Join-Path $package 'd3d9.dll'
if (-not (Test-Path -LiteralPath $packagedHook -PathType Leaf) -or
    (Get-LauncherHash $hookDll) -ne (Get-LauncherHash $packagedHook)) {
    throw 'The installed d3d9.dll does not match this VR package. Install the current package before launching.'
}
if ((Get-Content -LiteralPath $configPath) -match '^\s*enabled\s*=\s*0\s*(;.*)?$') {
    throw 'VR is disabled in the game-folder acah_vr.ini. Set enabled=1 before launching.'
}
if (-not (Get-Process -Name 'vrserver' -ErrorAction SilentlyContinue)) {
    throw 'Start SteamVR and connect the headset, then run this launcher again.'
}

$existingHosts = @(Get-Process -Name 'acah_vr_host' -ErrorAction SilentlyContinue)
foreach ($existing in $existingHosts) {
    # Only this package's host may be restarted; never terminate another installation.
    if (-not [string]::Equals($existing.Path, $hostExe, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Another VR host is running outside this package (PID $($existing.Id)). Close it before launching."
    }
}
$existingGame = @(Get-Process -Name 'Ace Combat_AH' -ErrorAction SilentlyContinue)
foreach ($existing in $existingGame) {
    if (-not [string]::Equals($existing.Path, $gameExe, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Another Ace Combat installation is running (PID $($existing.Id)). Close it before launching."
    }
}
Write-Host "SteamVR runtime: $runtime"
Write-Host 'Mode: stereo requested automatically. Menus may use the virtual screen until mission rendering begins.'
if ($CheckOnly) {
    Write-Host "Checks passed. Would start $hostExe --stereo and open/reuse $gameExe."
    Write-Host "Existing package hosts to restart: $($existingHosts.Count); running game instances to reuse: $($existingGame.Count)."
    return
}

foreach ($existing in $existingHosts) {
    Write-Host "Restarting this package's VR host (PID $($existing.Id)) in stereo mode."
    Stop-Process -InputObject $existing -Force
    if (-not $existing.WaitForExit(5000)) { throw 'Previous VR host has not exited.' }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$stdoutLog = Join-Path $package "launch-$stamp.host.stdout.log"
$stderrLog = Join-Path $package "launch-$stamp.host.stderr.log"
$previousRuntime = $env:XR_RUNTIME_JSON
$hostProcess = $null
try {
    try {
        # The child inherits this runtime override. No system registry changes.
        $env:XR_RUNTIME_JSON = $runtime
        $hostProcess = Start-Process -FilePath $hostExe -ArgumentList '--stereo' -WorkingDirectory $package `
            -WindowStyle Hidden -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog -PassThru
    } finally {
        if ($null -eq $previousRuntime) { Remove-Item Env:\XR_RUNTIME_JSON -ErrorAction SilentlyContinue }
        else { $env:XR_RUNTIME_JSON = $previousRuntime }
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $ready = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($hostProcess.HasExited) {
            $details = if (Test-Path -LiteralPath $stderrLog) { Get-Content -LiteralPath $stderrLog -Raw } else { '' }
            throw "VR host exited with code $($hostProcess.ExitCode). $details See $stdoutLog"
        }
        if ((Test-Path -LiteralPath $stdoutLog) -and
            (Get-Content -LiteralPath $stdoutLog -Raw) -match '(?m)^Ready\.') {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw "VR host did not initialize within 30 seconds. See $stdoutLog and $stderrLog" }
    if (-not (Get-Process -Name 'Ace Combat_AH' -ErrorAction SilentlyContinue)) {
        # Explicit game working directory is required by this installation.
        $null = Start-Process -FilePath $gameExe -WorkingDirectory $gameFolder -PassThru
        Write-Host 'Ace Combat launched with the VR host ready.'
    } else {
        Write-Host 'The running Ace Combat instance will connect to the restarted VR host.'
    }
    # The launcher console can take focus back when it closes. Activate the
    # game once after its window exists; never keep stealing focus in a loop.
    $gameProcess = Get-Process -Name 'Ace Combat_AH' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($gameProcess) {
        $windowDeadline = [DateTime]::UtcNow.AddSeconds(5)
        while (-not $gameProcess.HasExited -and -not $gameProcess.MainWindowHandle -and [DateTime]::UtcNow -lt $windowDeadline) {
            Start-Sleep -Milliseconds 100
            $gameProcess.Refresh()
        }
        if ($gameProcess.MainWindowHandle) {
            try {
                $windowShell = New-Object -ComObject WScript.Shell
                $null = $windowShell.AppActivate($gameProcess.Id)
            } catch { Write-Host 'Select the game window once so fullscreen rendering keeps focus.' }
        }
    }
} catch {
    # Only clean up the helper created by this invocation when startup fails.
    if ($hostProcess -and -not $hostProcess.HasExited) { Stop-Process -InputObject $hostProcess -Force -ErrorAction SilentlyContinue }
    throw
}
Write-Host 'In the cockpit, face forward and press F9 to recenter. F10 returns to the screen; F8 toggles stereo.'
Write-Host 'Keep the game active. Ctrl+Shift+F12 exits the VR host.'
Write-Host "VR host status: $stdoutLog"
Write-Host "VR host errors: $stderrLog"
