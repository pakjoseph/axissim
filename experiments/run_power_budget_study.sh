#!/usr/bin/env bash
# Illustrative arithmetic-efficiency-to-capacity study; all power inputs are assumptions.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/power_budget_study.csv"}
config="$root/configs/power_budget/manifest.conf"
mkdir -p "$(dirname "$output")"
echo 'scenario,npu_arithmetic_energy_scale,arithmetic_voltage_ratio,npu_macs,dram_bandwidth_gbps,mean_latency_us,energy_per_frame_uj,average_it_power_w,average_facility_power_w,budget_status' > "$output"
run_case() {
  local name=$1 scale=$2 voltage=$3 macs=$4 bandwidth=$5
  local result_json
  result_json=$(mktemp)
  trap 'rm -f "$result_json"' RETURN
  "$binary" run "$config" --set npu_arithmetic_energy_scale="$scale" --set arithmetic_voltage_ratio="$voltage" --set npu_macs="$macs" --set dram_bandwidth_gbps="$bandwidth" --json "$result_json" >/dev/null
  local latency energy power facility status
  latency=$(jq -r '.latency.mean_us' "$result_json")
  energy=$(jq -r '.energy.it_per_frame_uj' "$result_json")
  power=$(jq -r '.power.average_it_w' "$result_json")
  facility=$(jq -r '.power.average_facility_w' "$result_json")
  status=$(jq -r 'if .power.feasible then "feasible" else "infeasible" end' "$result_json")
  echo "$name,$scale,$voltage,$macs,$bandwidth,$latency,$energy,$power,$facility,$status" >> "$output"
  rm -f "$result_json"
  trap - RETURN
}
run_case baseline 1 1 1024 20
run_case iso_performance 0.333333 1 1024 20
run_case voltage_sensitivity 1 0.666667 1024 20
run_case compute_reinvestment 0.333333 1 4096 20
run_case memory_constrained_compute 0.333333 1 4096 0.5
echo "Wrote 5 points to $output"
