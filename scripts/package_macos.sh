#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)
build_dir=${1:-"$project_root/build-release"}
app="$build_dir/AxisSim.app"
dmg="$build_dir/AxisSim.dmg"
staging="$build_dir/axissim-dmg-staging"

if ! command -v macdeployqt >/dev/null 2>&1; then
  echo "macdeployqt was not found. Install Qt 6 and ensure its bin directory is on PATH." >&2
  exit 1
fi

cmake -S "$project_root" -B "$build_dir" -DAXISSIM_BUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release
cmake -E remove_directory "$app"
cmake --build "$build_dir" --target axissim-gui --parallel

macdeployqt "$app" -always-overwrite
# AxisSim uses only the native Cocoa platform and macOS style plugins. Pruning
# optional format/input plugins prevents unused QtSvg/QtPdf/QML dependencies
# from falling back to a developer-machine Qt installation.
for plugin_dir in iconengines imageformats platforminputcontexts; do
  cmake -E remove_directory "$app/Contents/PlugIns/$plugin_dir"
done
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

cmake -E remove_directory "$staging"
cmake -E make_directory "$staging"
ditto "$app" "$staging/AxisSim.app"
ln -s /Applications "$staging/Applications"
hdiutil create -volname "AxisSim" -srcfolder "$staging" -ov -format UDZO "$dmg"
cmake -E remove_directory "$staging"

echo "Packaged app: $app"
echo "Disk image:   $dmg"
