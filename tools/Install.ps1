$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$game = [IO.Path]::GetFullPath((Join-Path $source '..'))
$package = Join-Path $source 'package'
$manifestPath = Join-Path $package 'install-manifest.json'
if (-not (Test-Path -LiteralPath (Join-Path $game 'Ace Combat_AH.exe'))) { throw 'Game executable was not found beside vr-poc.' }
if (Get-Process -Name 'Ace Combat_AH' -ErrorAction SilentlyContinue) { throw 'Close the game before installing.' }
if (Test-Path -LiteralPath $manifestPath) { throw 'An install manifest already exists. Run Remove.ps1 before reinstalling.' }
$files = @('d3d9.dll','acah_vr.ini')
foreach ($name in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $name))) { throw "Missing package file $name. Run Build.ps1 first." }
    if (Test-Path -LiteralPath (Join-Path $game $name)) { throw "Existing $name in the game folder. Preserve/remove it yourself before installing this proxy." }
}
$manifest = @{ game = $game; files = @() }
try {
    foreach ($name in $files) {
        $destination = Join-Path $game $name
        Copy-Item -LiteralPath (Join-Path $package $name) -Destination $destination
        $manifest.files += @{ name = $name; sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash }
    }
} finally {
    # Keep a record even if a copy fails part-way through.
    if ($manifest.files.Count) { $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 }
}
Write-Host "Installed proxy and configuration in $game. Game executable and archives were not patched."
