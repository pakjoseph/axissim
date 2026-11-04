#!/usr/bin/env bash
# Illustrative arithmetic-efficiency-to-capacity study; all power inputs are assumptions.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/power_budget_study.csv"}
config="$root/configs/power_budget/manifest.conf"
mkdir -p "$(dirname "$output")"
echo 'scenario,npu_arithmetic_energy_scale,npu_macs,dram_bandwidth_gbps,mean_latency_us,energy_per_frame_uj,average_it_power_w,average_facility_power_w,budget_status' > "$output"
run_case() {
  local name=$1 scale=$2 macs=$3 bandwidth=$4
  local result
  result=$("$binary" run "$config" --set npu_arithmetic_energy_scale="$scale" --set npu_macs="$macs" --set dram_bandwidth_gbps="$bandwidth")
  local latency energy power facility status
  latency=$(awk '/latency mean/ {print $5}' <<<"$result")
  energy=$(awk '/energy static\/IT\/facility/ {print $5}' <<<"$result")
  power=$(awk '/average IT\/facility power/ {print $4}' <<<"$result")
  facility=$(awk '/average IT\/facility power/ {print $6}' <<<"$result")
  status=$(awk '/IT power budget/ {gsub(/[()]/,"",$NF); print $NF}' <<<"$result")
  echo "$name,$scale,$macs,$bandwidth,$latency,$energy,$power,$facility,$status" >> "$output"
}
run_case baseline 1 1024 20
run_case iso_performance 0.333333 1024 20
run_case compute_reinvestment 0.333333 4096 20
run_case memory_constrained_compute 0.333333 4096 0.5
echo "Wrote 4 points to $output"
