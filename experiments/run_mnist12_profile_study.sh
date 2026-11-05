#!/usr/bin/env bash
# Public-model architecture study. Results are modeled estimates under the
# checked-in MNIST-12 profile and scenario assumptions, not silicon measurements.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/mnist12_profile_study.csv"}
config="$root/configs/workloads/mnist12_profiled.conf"

mkdir -p "$(dirname "$output")"
echo 'rvv_lanes,npu_macs,dram_bandwidth_gbps,npu_arithmetic_energy_scale,mean_latency_us,deadline_misses,energy_per_frame_uj' > "$output"

for lanes in 2 4 8 16; do
  for macs in 256 512 1024 2048; do
    for bandwidth in 0.15 0.5 5 20; do
      for scale in 1 0.333333; do
        result=$("$binary" run "$config" --set rvv_lanes="$lanes" --set npu_macs="$macs" --set dram_bandwidth_gbps="$bandwidth" --set npu_arithmetic_energy_scale="$scale")
        latency=$(awk '/latency mean/ {print $5}' <<<"$result")
        misses=$(awk '/deadline misses/ {print $3}' <<<"$result")
        energy=$(awk '/energy per frame/ {print $4}' <<<"$result")
        echo "$lanes,$macs,$bandwidth,$scale,$latency,$misses,$energy" >> "$output"
      done
    done
  done
done

echo "Wrote 128 points to $output"
