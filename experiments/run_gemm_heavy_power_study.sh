#!/usr/bin/env bash
# Contrasts arithmetic-energy propagation in an illustrative GEMM-heavy workload.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/gemm_heavy_power_study.csv"}
config="$root/configs/power_budget/manifest.conf"
mkdir -p "$(dirname "$output")"
echo 'scenario,npu_arithmetic_energy_scale,npu_macs,mean_latency_us,it_energy_per_frame_uj,average_it_power_w,budget_status' > "$output"
for row in 'baseline,1,1024' 'iso_performance,0.333333,1024' 'compute_reinvestment,0.333333,4096'; do
  IFS=, read -r name scale macs <<< "$row"
  result_json=$(mktemp)
  "$binary" run "$config" --set npu_arithmetic_energy_scale="$scale" --set npu_macs="$macs" --set gemm_m=512 --set gemm_n=512 --set gemm_k=512 --set npu_tile_m=128 --set npu_tile_n=128 --set npu_tile_k=128 --set sram_capacity_bytes=131072 --set npu_weights_resident=1 --set weight_sram_capacity_bytes=524288 --set npu_mac_pj=2.0 --set dram_byte_pj=5.0 --json "$result_json" >/dev/null
  latency=$(jq -r '.latency.mean_us' "$result_json")
  energy=$(jq -r '.energy.it_per_frame_uj' "$result_json")
  power=$(jq -r '.power.average_it_w' "$result_json")
  status=$(jq -r 'if .power.feasible then "feasible" else "infeasible" end' "$result_json")
  rm -f "$result_json"
  echo "$name,$scale,$macs,$latency,$energy,$power,$status" >> "$output"
done
echo "Wrote 3 points to $output"
