# Creates a portable Windows ZIP after a native Qt 6/MSVC build. Run from a
# Developer PowerShell with windeployqt available on PATH.
param([string]$BuildDir = "build/package-windows-release", [string]$ReleaseTag = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root $BuildDir
$Stage = Join-Path $BuildDir "portable"
$Archive = Join-Path $BuildDir "AxisSim-windows-x64.zip"

cmake -S $Root -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON "-DAXISSIM_RELEASE_TAG=$ReleaseTag"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $BuildDir --target axissim-gui --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
windeployqt (Join-Path $Stage "AxisSim.exe") --release --no-translations --compiler-runtime
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
$Revision = (git -C $Root rev-parse HEAD).Trim()
python (Join-Path $Root "tools/generate_package_runtime_inventory.py") `
  --root $Stage --platform windows --qt-version (python -c "import json,pathlib,sys; print(json.loads(pathlib.Path(sys.argv[1]).read_text()).get('build',{}).get('qt_version','unknown'))" (Join-Path $BuildDir "package-assets/package-provenance.json")) `
  --runtime-mode bundled-dynamic --output-dir (Join-Path $Stage "third-party")
if ($LASTEXITCODE -ne 0) { throw "runtime inventory generation failed with exit code $LASTEXITCODE" }
python (Join-Path $Root "tools/verify_extracted_package.py") --root $Stage --platform windows --layout portable `
  --expected-source-revision $Revision --gui-smoke
if ($LASTEXITCODE -ne 0) { throw "portable Windows stage verification failed with exit code $LASTEXITCODE" }
if (Test-Path $Archive) { Remove-Item $Archive }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Archive
$Extracted = Join-Path ([System.IO.Path]::GetTempPath()) ("axissim-windows-package-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Extracted | Out-Null
try {
  Expand-Archive -Path $Archive -DestinationPath $Extracted
  python (Join-Path $Root "tools/verify_extracted_package.py") --root $Extracted --platform windows --layout portable `
    --expected-source-revision $Revision --gui-smoke
  if ($LASTEXITCODE -ne 0) { throw "freshly extracted Windows ZIP verification failed with exit code $LASTEXITCODE" }
} finally {
  if (Test-Path $Extracted) { Remove-Item $Extracted -Recurse -Force }
}
Write-Output "Portable Windows archive: $Archive"
