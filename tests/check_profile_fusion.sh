#!/usr/bin/env bash
set -euo pipefail

binary=$1
baseline_config=$2
fused_config=$3
baseline=$(mktemp)
fused=$(mktemp)
trap 'rm -f "$baseline" "$fused"' EXIT

"$binary" run "$baseline_config" --json "$baseline" >/dev/null
"$binary" run "$fused_config" --json "$fused" >/dev/null

jq -e '
  .mapping_effects.fusion_count == 1 and
  .mapping_effects.fused_intermediate_bytes_avoided_per_frame == 50176 and
  .profile_transfer_ledger.per_frame_bytes == 240000 and
  .traffic.dram_bytes == 7200000 and
  .energy.it_per_frame_uj < 4.2 and
  .latency.mean_us < 54.0
' "$fused" >/dev/null

baseline_traffic=$(jq -r '.traffic.dram_bytes' "$baseline")
fused_traffic=$(jq -r '.traffic.dram_bytes' "$fused")
[[ $((baseline_traffic - fused_traffic)) == $((50176 * 30)) ]]
