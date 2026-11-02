#!/usr/bin/env bash
# Reproduce a single public Ara fmatmul64 hardware-cycle point without modifying Ara sources.
# Usage: tools/reproduce_ara_fmatmul.sh --ara-dir /path/to/ara --lanes 2 --output-dir /path/to/output [--build]
set -euo pipefail

readonly PINNED_ARA_REVISION="34bd3bc152421b4601a7bf3d6e8e91ffb545c99e"
readonly REQUIRED_FREE_KIB=$((20 * 1024 * 1024))

ara_dir=""
lanes=""
output_dir=""
build=0

usage() {
  echo "Usage: $0 --ara-dir <public Ara checkout> --lanes <2|4|8|16> --output-dir <directory> [--build]" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ara-dir) ara_dir=${2:-}; shift 2 ;;
    --lanes) lanes=${2:-}; shift 2 ;;
    --output-dir) output_dir=${2:-}; shift 2 ;;
    --build) build=1; shift ;;
    *) usage ;;
  esac
done

[[ -n "$ara_dir" && -n "$lanes" && -n "$output_dir" ]] || usage
[[ "$lanes" =~ ^(2|4|8|16)$ ]] || { echo "lanes must be 2, 4, 8, or 16" >&2; exit 2; }
ara_dir=$(cd "$ara_dir" && pwd)
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)

[[ -d "$ara_dir/.git" ]] || { echo "Ara directory is not a Git checkout: $ara_dir" >&2; exit 1; }
revision=$(git -C "$ara_dir" rev-parse HEAD)
[[ "$revision" == "$PINNED_ARA_REVISION" ]] || {
  echo "Ara revision must be $PINNED_ARA_REVISION; found $revision" >&2
  exit 1
}

available_kib=$(df -Pk "$output_dir" | awk 'NR == 2 { print $4 }')
if (( available_kib < REQUIRED_FREE_KIB )); then
  echo "Need at least 20 GiB free to build Ara RTL/toolchains; only $((available_kib / 1024 / 1024)) GiB is available." >&2
  exit 1
fi

clang_path="$ara_dir/install/riscv-llvm/bin/clang"
if [[ ! -x "$clang_path" ]]; then
  echo "Missing pinned Ara LLVM toolchain at $clang_path. Follow docs/ara-reproduction.md first." >&2
  exit 1
fi

if [[ "$build" == 1 ]]; then
  command -v verilator >/dev/null || { echo "--build requires Verilator on PATH" >&2; exit 1; }
  make -C "$ara_dir/hardware" bender
  make -C "$ara_dir/hardware" apply-patches
  make -C "$ara_dir/hardware" config="${lanes}_lanes" veril_path="$(dirname "$(command -v verilator)")" verilate
fi

simulator="$ara_dir/hardware/build/verilator/Vara_tb_verilator"
[[ -x "$simulator" ]] || { echo "Missing Verilated Ara simulator: $simulator. Re-run with --build after preparing prerequisites." >&2; exit 1; }

mkdir -p "$ara_dir/apps/benchmarks/data"
python3 "$ara_dir/apps/fmatmul/script/gen_data.py" 256 256 256 > "$ara_dir/apps/benchmarks/data/data.S"
config="${lanes}_lanes" ENV_DEFINES="-DFMATMUL=1" make -C "$ara_dir/apps" bin/benchmarks

raw_log="$output_dir/ara_fmatmul64_256_lanes${lanes}.raw.log"
metadata="$output_dir/ara_fmatmul64_256_lanes${lanes}.metadata.txt"
config="${lanes}_lanes" make -C "$ara_dir/hardware" simv app=benchmarks > "$raw_log"

hardware_cycles=$(awk -F: '/\[hw-cycles\]/ { gsub(/ /, "", $2); print $2; exit }' "$raw_log")
[[ "$hardware_cycles" =~ ^[0-9]+$ ]] || { echo "No [hw-cycles] marker in $raw_log" >&2; exit 1; }

command_suffix=""
if [[ "$build" == 1 ]]; then command_suffix=" --build"; fi

{
  echo "source_url=https://github.com/pulp-platform/ara"
  echo "source_revision=$revision"
  echo "benchmark=fmatmul64_256x256"
  echo "lanes=$lanes"
  echo "matrix_dimensions=256x256x256"
  echo "hardware_cycles=$hardware_cycles"
  echo "raw_log=$raw_log"
  echo "command=$0 --ara-dir $ara_dir --lanes $lanes --output-dir $output_dir$command_suffix"
} > "$metadata"

echo "Recorded $hardware_cycles hardware cycles"
echo "Raw log: $raw_log"
echo "Metadata: $metadata"
