#!/usr/bin/env bash
set -euo pipefail
binary=$1
config=$2
ranges=$3
csv=$(mktemp)
json=$(mktemp)
trap 'rm -f "$csv" "$json"' EXIT
"$binary" analyze "$config" --ranges "$ranges" --csv "$csv" --json "$json" >/dev/null
[[ $(wc -l < "$csv" | tr -d ' ') == 16 ]]
jq -e '
  .schema_version == "axissim.sensitivity.v1" and
  .summary.point_count == 15 and
  (.interpretation | contains("not statistical confidence intervals"))
' "$json" >/dev/null
grep -q '^one_at_a_time,dram_bandwidth_gbps,low,' "$csv"
grep -q '^corner,all,' "$csv"
