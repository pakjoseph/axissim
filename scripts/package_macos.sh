#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)
build_dir=${1:-"$project_root/build/package-macos-release"}
release_tag=${2:-""}
app="$build_dir/AxisSim.app"
dmg="$build_dir/AxisSim.dmg"
staging="$build_dir/axissim-dmg-staging"
verification_root=""
mount_point=""

cleanup_package_validation() {
  local status=$?
  if [[ -n "$mount_point" ]]; then
    hdiutil detach "$mount_point" >/dev/null 2>&1 || true
  fi
  if [[ -n "$verification_root" ]]; then
    rm -rf "$verification_root"
  fi
  if [[ $status -ne 0 ]]; then
    rm -f "$dmg"
    rm -rf "$app" "$staging"
  fi
}
trap cleanup_package_validation EXIT
mkdir -p "$build_dir"
rm -f "$dmg"
rm -rf "$app" "$staging"

if ! command -v macdeployqt >/dev/null 2>&1; then
  echo "macdeployqt was not found. Install Qt 6 and ensure its bin directory is on PATH." >&2
  exit 1
fi

cmake -S "$project_root" -B "$build_dir" -DAXISSIM_BUILD_GUI=ON -DAXISSIM_PACKAGE_BUILD=ON -DCMAKE_BUILD_TYPE=Release "-DAXISSIM_RELEASE_TAG=$release_tag"
cmake --build "$build_dir" --target axissim-gui --parallel

macdeployqt "$app" -always-overwrite 2>&1 | tee "$build_dir/macdeployqt.log"
# AxisSim uses only the native Cocoa platform and macOS style plugins. Pruning
# optional format/input plugins prevents unused QtSvg/QtPdf/QML dependencies
# from falling back to a developer-machine Qt installation.
for plugin_dir in iconengines imageformats platforminputcontexts; do
  cmake -E remove_directory "$app/Contents/PlugIns/$plugin_dir"
done
python3 "$project_root/tools/verify_macos_bundle.py" --contents "$app/Contents"
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
# Inventory the final signed runtime bytes. Then refresh the outer app signature
# so it seals the inventory without re-signing nested frameworks and changing
# the hashes just recorded for them.
python3 "$project_root/tools/generate_package_runtime_inventory.py" \
  --root "$app/Contents" --platform macos --qt-version "$(python3 -c 'import json,pathlib,sys; print(json.loads(pathlib.Path(sys.argv[1]).read_text()).get("build",{}).get("qt_version","unknown"))' "$build_dir/package-assets/package-provenance.json")" \
  --runtime-mode bundled-dynamic --output-dir "$app/Contents/Resources/axissim-package/third-party"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"

cmake -E remove_directory "$staging"
cmake -E make_directory "$staging"
ditto "$app" "$staging/AxisSim.app"
ln -s /Applications "$staging/Applications"
hdiutil create -volname "AxisSim" -srcfolder "$staging" -ov -format UDZO "$dmg"
cmake -E remove_directory "$staging"

expected_revision=$(git -C "$project_root" rev-parse HEAD)
verification_root=$(mktemp -d "${TMPDIR:-/tmp}/axissim-macos-package.XXXXXX")
mount_point="$verification_root/mounted"
relocated_app="$verification_root/relocated/AxisSim.app"
mkdir -p "$mount_point" "$(dirname "$relocated_app")"
hdiutil attach -nobrowse -readonly -mountpoint "$mount_point" "$dmg"
ditto "$mount_point/AxisSim.app" "$relocated_app"
hdiutil detach "$mount_point"
mount_point=""
python3 "$project_root/tools/verify_macos_bundle.py" --contents "$relocated_app/Contents"
python3 "$project_root/tools/verify_extracted_package.py" --root "$relocated_app/Contents" --platform macos --layout macos \
  --expected-source-revision "$expected_revision" --gui-smoke
echo "Packaged app: $app"
echo "Disk image:   $dmg"
