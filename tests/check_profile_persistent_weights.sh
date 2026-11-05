#!/usr/bin/env bash
set -euo pipefail

binary=$1
baseline_config=$2
persistent_config=$3
baseline=$(mktemp)
persistent=$(mktemp)
trap 'rm -f "$baseline" "$persistent"' EXIT

"$binary" run "$baseline_config" --json "$baseline" >/dev/null
"$binary" run "$persistent_config" --json "$persistent" >/dev/null

jq -e '
  .mapping_effects.persistent_weight_operator_count == 2 and
  .mapping_effects.persistent_weight_bytes_avoided_per_steady_frame == 13600 and
  .profile_transfer_ledger.first_frame_bytes == 290176 and
  .profile_transfer_ledger.steady_state_bytes == 276576 and
  .traffic.dram_bytes == 8310880 and
  .energy.it_per_frame_uj < 4.8
' "$persistent" >/dev/null

baseline_traffic=$(jq -r '.traffic.dram_bytes' "$baseline")
persistent_traffic=$(jq -r '.traffic.dram_bytes' "$persistent")
[[ $((baseline_traffic - persistent_traffic)) == $((13600 * 29)) ]]
