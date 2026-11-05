#!/usr/bin/env bash
set -euo pipefail

binary=$1
baseline_config=$2
precision_config=$3
baseline=$(mktemp)
precision=$(mktemp)
trap 'rm -f "$baseline" "$precision"' EXIT

"$binary" run "$baseline_config" --json "$baseline" >/dev/null
"$binary" run "$precision_config" --json "$precision" >/dev/null

jq -e '
  .mapping_effects.source_storage_precision == "fp32" and
  .mapping_effects.target_storage_precision == "fp16" and
  .mapping_effects.precision_traffic_bytes_avoided_per_frame == 145088 and
  .mapping_effects.mapped_npu_arithmetic_energy_scale == 0.5 and
  .profile_transfer_ledger.first_frame_bytes == 145088 and
  .profile_transfer_ledger.steady_state_bytes == 145088 and
  .traffic.dram_bytes == 4352640 and
  .energy.npu_arithmetic_pj == 2359680
' "$precision" >/dev/null

baseline_traffic=$(jq -r '.traffic.dram_bytes' "$baseline")
precision_traffic=$(jq -r '.traffic.dram_bytes' "$precision")
[[ $((baseline_traffic - precision_traffic)) == $((145088 * 30)) ]]
