#!/bin/sh
# Reproduces the three initial AxisSim architecture studies.
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary="$project_dir/build/axissim"

if [ ! -x "$binary" ]; then
  echo "AxisSim binary not found. Build it first: cmake -S . -B build && cmake --build build" >&2
  exit 1
fi

"$binary" sweep "$project_dir/configs/baseline/manifest.conf" \
  --sweep dram_bandwidth_gbps=0.15,0.5,1,5,20 \
  --csv "$project_dir/experiments/bandwidth_sweep.csv"

"$binary" sweep "$project_dir/configs/baseline/manifest.conf" \
  --sweep npu_macs=256,512,1024,2048,4096 \
  --csv "$project_dir/experiments/npu_macs_sweep.csv"

"$binary" sweep "$project_dir/configs/baseline/manifest.conf" \
  --sweep rvv_lanes=2,4,8,16,32 \
  --csv "$project_dir/experiments/rvv_lanes_sweep.csv"

"$binary" sweep "$project_dir/configs/baseline/manifest.conf" \
  --sweep rvv_vlen_bits=128,256,512,1024 \
  --csv "$project_dir/experiments/rvv_vlen_sweep.csv"

"$binary" sweep "$project_dir/configs/baseline/manifest.conf" \
  --sweep npu_arithmetic_energy_scale=1,0.5,0.333333,0.25 \
  --csv "$project_dir/experiments/arithmetic_energy_sweep.csv"

echo "Initial study complete. Results written to $project_dir/experiments/*.csv"
