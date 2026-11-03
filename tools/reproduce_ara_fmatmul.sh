#!/usr/bin/env bash
# Reproduce a single public Ara fmatmul64 hardware-cycle point without modifying Ara sources.
# Usage: tools/reproduce_ara_fmatmul.sh --ara-dir /path/to/ara --lanes 2 --output-dir /path/to/output [--matrix-size 128] [--build]
set -euo pipefail

readonly PINNED_ARA_REVISION="34bd3bc152421b4601a7bf3d6e8e91ffb545c99e"
readonly REQUIRED_FREE_KIB=$((20 * 1024 * 1024))

ara_dir=""
lanes=""
output_dir=""
matrix_size=128
build=0

usage() {
  echo "Usage: $0 --ara-dir <public Ara checkout> --lanes <2|4|8|16> --output-dir <directory> [--matrix-size <1..128>] [--build]" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ara-dir) ara_dir=${2:-}; shift 2 ;;
    --lanes) lanes=${2:-}; shift 2 ;;
    --output-dir) output_dir=${2:-}; shift 2 ;;
    --matrix-size) matrix_size=${2:-}; shift 2 ;;
    --build) build=1; shift ;;
    *) usage ;;
  esac
done

[[ -n "$ara_dir" && -n "$lanes" && -n "$output_dir" ]] || usage
[[ "$lanes" =~ ^(2|4|8|16)$ ]] || { echo "lanes must be 2, 4, 8, or 16" >&2; exit 2; }
[[ "$matrix_size" =~ ^[0-9]+$ ]] && (( matrix_size >= 1 && matrix_size <= 128 )) || {
  echo "matrix-size must be an integer from 1 through 128 for Ara's unmodified 1 MiB testbench RAM" >&2
  exit 2
}
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

verilator_path="${AXISSIM_VERILATOR_PATH:-$ara_dir/install/verilator/bin/verilator}"
if [[ ! -x "$verilator_path" ]] && command -v verilator >/dev/null; then
  verilator_path=$(command -v verilator)
fi

if [[ "$build" == 1 ]]; then
  [[ -x "$verilator_path" ]] || { echo "--build requires Ara's installed Verilator or AXISSIM_VERILATOR_PATH" >&2; exit 1; }
  make -C "$ara_dir/hardware" bender
  make -C "$ara_dir/hardware" checkout
  make -C "$ara_dir/hardware" apply-patches
  make -C "$ara_dir/hardware" config="${lanes}_lanes" veril_path="$(dirname "$verilator_path")" verilate
fi

simulator="$ara_dir/hardware/build/verilator/Vara_tb_verilator"
[[ -x "$simulator" ]] || { echo "Missing Verilated Ara simulator: $simulator. Re-run with --build after preparing prerequisites." >&2; exit 1; }

mkdir -p "$ara_dir/apps/benchmarks/data"
data_file="$ara_dir/apps/benchmarks/data/data.S"
python3 "$ara_dir/apps/fmatmul/script/gen_data.py" "$matrix_size" "$matrix_size" "$matrix_size" > "$data_file"
config="${lanes}_lanes" ENV_DEFINES="-DFMATMUL=1" make -C "$ara_dir/apps" bin/benchmarks

raw_log="$output_dir/ara_fmatmul64_${matrix_size}_lanes${lanes}.raw.log"
metadata="$output_dir/ara_fmatmul64_${matrix_size}_lanes${lanes}.metadata.txt"
config="${lanes}_lanes" make -C "$ara_dir/hardware" simv app=benchmarks > "$raw_log"

hardware_cycles=$(awk -F: '/\[hw-cycles\]/ { gsub(/ /, "", $2); print $2; exit }' "$raw_log")
[[ "$hardware_cycles" =~ ^[0-9]+$ ]] || { echo "No [hw-cycles] marker in $raw_log" >&2; exit 1; }

hash_file() {
  if command -v sha256sum >/dev/null; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

hash_stdin() {
  if command -v sha256sum >/dev/null; then
    sha256sum | awk '{print $1}'
  else
    shasum -a 256 | awk '{print $1}'
  fi
}

config_file="$ara_dir/config/${lanes}_lanes.mk"
elf_file="$ara_dir/apps/bin/benchmarks"
submodule_state_hash=$(git -C "$ara_dir" submodule status --recursive | hash_stdin)

{
  echo "source_url=https://github.com/pulp-platform/ara"
  echo "source_revision=$revision"
  echo "benchmark=fmatmul64_${matrix_size}x${matrix_size}"
  echo "data_type=fp64"
  echo "lanes=$lanes"
  echo "matrix_dimensions=${matrix_size}x${matrix_size}x${matrix_size}"
  echo "ara_config_sha256=$(hash_file "$config_file")"
  echo "ara_submodule_state_sha256=$submodule_state_hash"
  echo "compiler=$("$clang_path" --version | head -1)"
  echo "verilator=$("$verilator_path" --version 2>/dev/null | head -1 || echo unavailable)"
  echo "host_os=$(uname -s)"
  echo "host_arch=$(uname -m)"
  echo "execution_context=${AXISSIM_EXECUTION_CONTEXT:-native}"
  echo "container_image_digest=${AXISSIM_CONTAINER_IMAGE_DIGEST:-not-recorded}"
  echo "data_sha256=$(hash_file "$data_file")"
  echo "benchmark_elf_sha256=$(hash_file "$elf_file")"
  echo "hardware_cycles=$hardware_cycles"
  echo "raw_log_sha256=$(hash_file "$raw_log")"
  echo "raw_log_filename=$(basename "$raw_log")"
  echo "command=$0 --ara-dir $ara_dir --lanes $lanes --output-dir $output_dir --matrix-size $matrix_size$([[ "$build" == 1 ]] && printf ' --build')"
} > "$metadata"

echo "Recorded $hardware_cycles hardware cycles"
echo "Raw log: $raw_log"
echo "Metadata: $metadata"
