param([switch]$RemoveEditedConfig)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$game = [IO.Path]::GetFullPath((Join-Path $source '..'))
$manifestPath = Join-Path $source 'package\install-manifest.json'
if (Get-Process -Name 'Ace Combat_AH' -ErrorAction SilentlyContinue) { throw 'Close the game before removing the proxy.' }
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'No install manifest was found; nothing was removed.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not [string]::Equals([IO.Path]::GetFullPath($manifest.game),$game,[StringComparison]::OrdinalIgnoreCase)) { throw 'Install manifest points to a different game folder.' }
$remaining = @()
foreach ($file in $manifest.files) {
    if ($file.name -notin @('d3d9.dll','acah_vr.ini')) { throw 'Unexpected file in install manifest.' }
    $target = [IO.Path]::GetFullPath((Join-Path $game $file.name))
    if (-not [string]::Equals([IO.Path]::GetDirectoryName($target),$game,[StringComparison]::OrdinalIgnoreCase)) { throw 'Removal target is outside the game folder.' }
    if (-not (Test-Path -LiteralPath $target)) { continue }
    $same = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $file.sha256
    if ($same -or ($file.name -eq 'acah_vr.ini' -and $RemoveEditedConfig)) { Remove-Item -LiteralPath $target }
    else { Write-Warning "Preserved modified $target. Remove it manually if intended."; $remaining += $file }
}
if ($remaining.Count) {
    @{ game = $game; files = $remaining } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
} else { Remove-Item -LiteralPath $manifestPath }
Write-Host 'Proxy removal finished. Analysis, source, package, and logs were retained.'
