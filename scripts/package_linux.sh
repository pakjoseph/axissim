#!/usr/bin/env bash
# Creates a portable Linux archive and a Debian/Ubuntu package after a
# Qt-enabled native build. The `.deb` declares its Qt runtime dependencies and
# installs a desktop launcher; an AppImage is intentionally outside scope.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$root/build-linux-release"}
release_tag=${2:-""}
if [[ "$build_dir" != /* ]]; then
  build_dir="$root/$build_dir"
fi
stage="$build_dir/portable"
archive="$build_dir/AxisSim-linux-$(uname -m).tar.gz"

cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON "-DAXISSIM_RELEASE_TAG=$release_tag"
cmake --build "$build_dir" --target axissim-gui --parallel
test -x "$stage/AxisSim"
test -x "$stage/axissim-cli"
revision=$(git -C "$root" rev-parse HEAD)
python3 "$root/tools/generate_package_runtime_inventory.py" \
  --root "$stage" --platform linux --qt-version "$(python3 -c 'import json,pathlib,sys; p=pathlib.Path(sys.argv[1]); print(json.loads(p.read_text()).get("build",{}).get("qt_version","unknown"))' "$build_dir/package-assets/package-provenance.json")" \
  --runtime-mode system-dynamic --output-dir "$build_dir/package-assets/third-party"
cmake -E copy_directory "$build_dir/package-assets" "$stage"
verify_tar="$build_dir/verify-tar"
verify_deb="$build_dir/verify-deb"
cmake -E remove_directory "$verify_tar"
cmake -E remove_directory "$verify_deb"
cmake -E make_directory "$verify_tar" "$verify_deb"
tar -C "$stage" -czf "$archive" .
tar -C "$verify_tar" -xzf "$archive"
python3 "$root/tools/verify_extracted_package.py" --root "$verify_tar" --platform linux --layout portable \
  --expected-source-revision "$revision" --gui-smoke
cpak_config="$build_dir/CPackConfig.cmake"
test -f "$cpak_config"
if ! (cd "$build_dir" && cpack --config "$cpak_config" -G DEB); then
  preinstall_log=$(find "$build_dir/_CPack_Packages" -type f -name PreinstallOutput.log -print -quit)
  if [[ -n "$preinstall_log" ]]; then
    echo "CPack preinstall diagnostics ($preinstall_log):" >&2
    cat "$preinstall_log" >&2
  fi
  exit 1
fi
deb=$(find "$build_dir" -maxdepth 1 -type f -name '*.deb' -print -quit)
test -n "$deb"
dpkg-deb -x "$deb" "$verify_deb"
python3 "$root/tools/verify_extracted_package.py" --root "$verify_deb" --platform linux --layout deb \
  --expected-source-revision "$revision" --deb-file "$deb" --gui-smoke
echo "Portable Linux archive: $archive"
echo "Debian/Ubuntu package: $build_dir"/*.deb
