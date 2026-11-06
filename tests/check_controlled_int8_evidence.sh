#!/usr/bin/env bash
set -euo pipefail

binary=$1
config=$2
result=$(mktemp)
trap 'rm -f "$result"' EXIT

"$binary" run "$config" --json "$result" >/dev/null
jq -e '
  .accuracy_evidence.status == "external_evaluated" and
  .accuracy_evidence.metric == "top1_accuracy_fraction" and
  .accuracy_evidence.value == 0.9891 and
  (.accuracy_evidence.dataset | contains("10,000 IDX examples")) and
  .mapping_effects.source_storage_precision == "fp32" and
  .mapping_effects.target_storage_precision == "int8" and
  .mapping_effects.mapped_npu_arithmetic_energy_scale == 0.25 and
  .traffic.dram_bytes == 2176320 and
  .energy.it_per_frame_uj < 1.3
' "$result" >/dev/null
