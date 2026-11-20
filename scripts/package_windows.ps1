# Creates a portable Windows ZIP after a native Qt 6/MSVC build. Run from a
# Developer PowerShell with windeployqt available on PATH.
param(
  [string]$BuildDir = "build/package-windows-release",
  [string]$ReleaseTag = "",
  [switch]$InstallVcRuntimeForValidation
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root $BuildDir
$Stage = Join-Path $BuildDir "portable"
$Archive = Join-Path $BuildDir "AxisSim-windows-x64.zip"

if ([string]::IsNullOrWhiteSpace($env:VCINSTALLDIR) -or -not (Test-Path $env:VCINSTALLDIR)) {
  throw "VCINSTALLDIR must identify an installed Visual Studio C++ toolchain; run from a Visual Studio developer shell."
}
Write-Output "Using Visual Studio C++ runtime source: $env:VCINSTALLDIR"

cmake -S $Root -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON "-DAXISSIM_RELEASE_TAG=$ReleaseTag"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $BuildDir --target axissim-gui --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
windeployqt (Join-Path $Stage "AxisSim.exe") --release --no-translations --compiler-runtime
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }
$RedistInstallers = @(Get-ChildItem -Path $Stage -Filter "vc_redist.x64.exe" -File -Recurse)
if ($RedistInstallers.Count -ne 1) {
  throw "windeployqt must stage exactly one vc_redist.x64.exe prerequisite; found $($RedistInstallers.Count)"
}
if ($InstallVcRuntimeForValidation) {
  Write-Output "Installing the VC redistributable bundled with this validation package"
  $InstallProcess = Start-Process -FilePath $RedistInstallers[0].FullName `
    -ArgumentList @("/install", "/quiet", "/norestart") -Wait -PassThru
  if ($InstallProcess.ExitCode -notin @(0, 3010, 1638)) {
    throw "Bundled VC redistributable installation failed with exit code $($InstallProcess.ExitCode)"
  }
  Write-Output "VC redistributable installer exited with $($InstallProcess.ExitCode)"
}
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
