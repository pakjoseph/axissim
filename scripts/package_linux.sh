#!/usr/bin/env bash
# Creates a portable Linux archive and a Debian/Ubuntu package after a
# Qt-enabled native build. The `.deb` declares its Qt runtime dependencies and
# installs a desktop launcher; an AppImage is intentionally outside scope.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$root/build-linux-release"}
if [[ "$build_dir" != /* ]]; then
  build_dir="$root/$build_dir"
fi
stage="$build_dir/portable"
archive="$build_dir/AxisSim-linux-$(uname -m).tar.gz"

cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON
cmake --build "$build_dir" --target axissim-gui --parallel
test -x "$stage/AxisSim"
test -x "$stage/axissim-cli"
tar -C "$stage" -czf "$archive" .
cpak_config="$build_dir/CPackConfig.cmake"
test -f "$cpak_config"
(cd "$build_dir" && cpack --config "$cpak_config" -G DEB)
echo "Portable Linux archive: $archive"
echo "Debian/Ubuntu package: $build_dir"/*.deb
