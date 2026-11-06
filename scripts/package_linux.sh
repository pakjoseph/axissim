#!/usr/bin/env bash
# Creates a portable Linux archive after a Qt-enabled native build. The
# resulting GUI uses the system Qt runtime; an AppImage/notarized installer is
# intentionally outside this script's scope.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$root/build-linux-release"}
stage="$build_dir/portable"
archive="$build_dir/AxisSim-linux-$(uname -m).tar.gz"

cmake -S "$root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DAXISSIM_BUILD_GUI=ON
cmake --build "$build_dir" --target axissim-gui --parallel
test -x "$stage/AxisSim"
test -x "$stage/axissim-cli"
tar -C "$stage" -czf "$archive" .
echo "Portable Linux archive: $archive"
