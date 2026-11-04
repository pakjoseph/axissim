#!/usr/bin/env bash
set -euo pipefail
binary=$1
config=$2
json=$(mktemp)
trap 'rm -f "$json"' EXIT
"$binary" run "$config" --set static_power_w=1 --set pue=1.5 --json "$json" >/dev/null
jq -e '
  (([.energy.vector_pj,.energy.npu_arithmetic_pj,.energy.dram_pj,.energy.dma_pj] | add) / 1000000) as $dynamic_uj |
  (($dynamic_uj - .energy.per_frame_uj) | fabs) < 0.000001 and
  (((.energy.per_frame_uj + .energy.static_per_frame_uj) - .energy.it_per_frame_uj) | fabs) < 0.000001 and
  (((.energy.it_per_frame_uj * 1.5) - .energy.facility_per_frame_uj) | fabs) < 0.000001 and
  (((.assumptions.base_static_power_w + .assumptions.npu_static_power_per_mac_w * 1) - .power.modeled_static_w) | fabs) < 0.000001
' "$json" >/dev/null
