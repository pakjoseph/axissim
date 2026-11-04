#!/usr/bin/env bash
# Reproducible Physical-AI architecture grid. Outputs modeled estimates, not silicon data.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/design_space.csv"}
config="$root/configs/baseline/manifest.conf"

mkdir -p "$(dirname "$output")"
echo 'rvv_lanes,dram_bandwidth_gbps,sram_capacity_bytes,npu_arithmetic_energy_scale,mean_latency_us,deadline_misses,energy_per_frame_uj' > "$output"

for lanes in 2 4 8 16; do
  for bandwidth in 0.5 1 5 20; do
    for sram in 131072 262144 524288; do
      for scale in 1 0.333333; do
        result=$("$binary" run "$config" --set rvv_lanes="$lanes" --set dram_bandwidth_gbps="$bandwidth" --set sram_capacity_bytes="$sram" --set npu_arithmetic_energy_scale="$scale")
        latency=$(awk '/latency mean/ {print $5}' <<<"$result")
        misses=$(awk '/deadline misses/ {print $3}' <<<"$result")
        energy=$(awk '/energy per frame/ {print $4}' <<<"$result")
        echo "$lanes,$bandwidth,$sram,$scale,$latency,$misses,$energy" >> "$output"
      done
    done
  done
done

echo "Wrote 96 points to $output"
