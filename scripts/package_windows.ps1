# Creates a portable Windows ZIP after a native Qt 6/MSVC build. Run from a
# Developer PowerShell with windeployqt available on PATH.
param([string]$BuildDir = "build-windows-release")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root $BuildDir
$Stage = Join-Path $BuildDir "portable"
$Archive = Join-Path $BuildDir "AxisSim-windows-x64.zip"

cmake -S $Root -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON
cmake --build $BuildDir --target axissim-gui --config Release --parallel
windeployqt (Join-Path $Stage "AxisSim.exe") --release --no-translations
if (Test-Path $Archive) { Remove-Item $Archive }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Archive
Write-Output "Portable Windows archive: $Archive"
