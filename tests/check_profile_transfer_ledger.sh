#!/usr/bin/env bash
set -euo pipefail

binary=$1
config=$2
result=$(mktemp)
trap 'rm -f "$result"' EXIT

"$binary" run "$config" --json "$result" >/dev/null

jq -e '
  .mapping_plan == "mnist12_baseline.json" and
  .profile_transfer_ledger.per_frame_bytes == 290176 and
  ([.profile_transfer_ledger.entries[].bytes] | add) == .profile_transfer_ledger.per_frame_bytes and
  .traffic.dram_bytes == (.profile_transfer_ledger.per_frame_bytes * 30) and
  ([.profile_transfer_ledger.entries[] | select(.category == "activation_read")] | length) > 0 and
  ([.profile_transfer_ledger.entries[] | select(.category == "activation_write")] | length) > 0 and
  ([.profile_transfer_ledger.entries[] | select(.category == "weight_read")] | length) > 0
' "$result" >/dev/null
