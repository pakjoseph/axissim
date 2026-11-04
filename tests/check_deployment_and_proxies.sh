#!/usr/bin/env bash
set -euo pipefail
binary=$1
config=$2
json=$(mktemp)
trap 'rm -f "$json"' EXIT

"$binary" run "$config" \
  --set static_power_w=1 \
  --set pue=1.5 \
  --set deployment_xpu_count=1000 \
  --set deployment_hours=1000 \
  --set electricity_cost_per_kwh=0.1 \
  --set base_area_mm2=1 \
  --set npu_area_per_mac_mm2=0.1 \
  --set rvv_area_per_lane_mm2=0.2 \
  --set sram_area_per_kib_mm2=0.3 \
  --set max_area_mm2=3 \
  --set ambient_temp_c=25 \
  --set thermal_resistance_c_per_w=10 \
  --set thermal_capacitance_j_per_c=1 \
  --set max_junction_temp_c=100 \
  --set reliability_fit_rate=100 \
  --set reliability_voltage_fit_multiplier=2 \
  --json "$json" >/dev/null

jq -e '
  def close($a; $b; $epsilon): (($a - $b) | fabs) < $epsilon;
  (.power.average_it_w * .power.pue * .deployment.xpu_count * .deployment.hours / 1000) as $deployment_kwh |
  (1 + 0.1 * 1 + 0.2 * 2 + 0.3 * (16 / 1024)) as $area_mm2 |
  (.power.average_it_w * 10 + 25) as $steady_c |
  (10 * 1) as $tau_s |
  ((.timing.makespan_us / 1000000) / $tau_s) as $normalized_time |
  (25 + ($steady_c - 25) * (1 - (-$normalized_time | exp))) as $end_c |
  (100 * 2 * 1000 * 1000 / 1000000000) as $failures |
  .schema_version == "1.1" and
  close(.deployment.electricity_cost_per_kwh; 0.1; 0.000001) and
  # Derived values use full-precision internals while JSON inputs are printed to
  # six decimals, so comparisons allow only the resulting serialization error.
  close(.deployment.facility_energy_kwh; $deployment_kwh; 0.001) and
  close(.deployment.electricity_cost; ($deployment_kwh * 0.1); 0.0001) and
  close(.area_proxy.modeled_mm2; $area_mm2; 0.00001) and
  close(.thermal_proxy.steady_state_c; $steady_c; 0.00001) and
  close(.thermal_proxy.end_c; $end_c; 0.00001) and
  close(.reliability_proxy.expected_failures; $failures; 0.000001)
' "$json" >/dev/null
