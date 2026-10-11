param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$hookBuild = Join-Path $source 'build-hook'
$hostBuild = Join-Path $source 'build-host'
$package = Join-Path $source 'package'
foreach ($command in @('cmake','git')) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { throw "$command must be on PATH." }
}
# This script intentionally compiles the source only when the user runs it.
& cmake -S $source -B $hookBuild -G 'Visual Studio 17 2022' -A Win32 '-DPOC_COMPONENT=hook'
if ($LASTEXITCODE -ne 0) { throw 'Hook configuration failed.' }
& cmake --build $hookBuild --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Hook build failed.' }
& cmake -S $source -B $hostBuild -G 'Visual Studio 17 2022' -A x64 '-DPOC_COMPONENT=host'
if ($LASTEXITCODE -ne 0) { throw 'Host configuration failed.' }
& cmake --build $hostBuild --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Host build failed.' }
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $hookBuild "$Configuration\d3d9.dll") -Destination $package
Copy-Item -LiteralPath (Join-Path $hostBuild "$Configuration\acah_vr_host.exe") -Destination $package
Copy-Item -LiteralPath (Join-Path $hostBuild "$Configuration\openxr_loader.dll") -Destination $package
Copy-Item -LiteralPath (Join-Path $source 'acah_vr.ini') -Destination $package
Write-Host "Package ready at $package. Run tools\Install.ps1 before testing the game."
