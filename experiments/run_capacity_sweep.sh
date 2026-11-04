#!/usr/bin/env bash
# Fixed-budget compute-vs-memory reinvestment sweep. All power values are illustrative assumptions.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/capacity_sweep.csv"}
config="$root/configs/power_budget/manifest.conf"
mkdir -p "$(dirname "$output")"
echo 'arithmetic_scale,npu_macs,dram_bandwidth_gbps,mapping,sram_capacity_bytes,tile_m,tile_n,tile_k,latency_us,it_energy_per_frame_uj,average_it_power_w,feasible' > "$output"

for scale in 1 0.333333; do
  for macs in 1024 2048 4096; do
    for bandwidth in 0.5 5 20; do
      for mapping in baseline large_reuse_tile; do
        if [[ $mapping == baseline ]]; then sram=131072; tm=64; tn=64; tk=128; else sram=524288; tm=256; tn=256; tk=256; fi
        json=$(mktemp)
        "$binary" run "$config" --set npu_arithmetic_energy_scale="$scale" --set npu_macs="$macs" --set dram_bandwidth_gbps="$bandwidth" --set sram_capacity_bytes="$sram" --set npu_tile_m="$tm" --set npu_tile_n="$tn" --set npu_tile_k="$tk" --json "$json" >/dev/null
        latency=$(jq -r '.latency.mean_us' "$json")
        energy=$(jq -r '.energy.it_per_frame_uj' "$json")
        power=$(jq -r '.power.average_it_w' "$json")
        feasible=$(jq -r '.power.feasible' "$json")
        echo "$scale,$macs,$bandwidth,$mapping,$sram,$tm,$tn,$tk,$latency,$energy,$power,$feasible" >> "$output"
        rm -f "$json"
      done
    done
  done
done
echo "Wrote 36 points to $output"
