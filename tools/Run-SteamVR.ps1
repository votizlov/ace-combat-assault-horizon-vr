param([string]$SteamVR = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR')
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$package = Join-Path $source 'package'
$hostExe = Join-Path $package 'acah_vr_host.exe'
$runtime = Join-Path $SteamVR 'steamxr_win64.json'
if (-not (Test-Path -LiteralPath $hostExe)) { throw 'Host executable is missing. Run Build.ps1 first.' }
if (-not (Test-Path -LiteralPath $runtime)) { throw 'SteamVR x64 runtime manifest was not found. Pass -SteamVR with its installation folder.' }
Write-Host 'Start SteamVR and connect the headset before continuing. The host starts in screen mode.'
$previous = $env:XR_RUNTIME_JSON
try {
    # Per-process loader override. The system runtime registry is left untouched.
    $env:XR_RUNTIME_JSON = $runtime
    & $hostExe --screen
    if ($LASTEXITCODE -ne 0) { throw "VR host exited with code $LASTEXITCODE. Read its console error." }
} finally {
    if ($null -eq $previous) { Remove-Item Env:\XR_RUNTIME_JSON -ErrorAction SilentlyContinue }
    else { $env:XR_RUNTIME_JSON = $previous }
}
