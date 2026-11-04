#!/usr/bin/env bash
# Pareto-filter feasible designs on lower latency and lower IT energy/frame.
set -euo pipefail
input=${1:-"$(cd "$(dirname "$0")" && pwd)/capacity_sweep.csv"}
output=${2:-"$(cd "$(dirname "$0")" && pwd)/capacity_pareto.csv"}
awk -F, '
  NR==1 { header=$0; next }
  $12=="true" { rows[++n]=$0; latency[n]=$9+0; energy[n]=$10+0 }
  END {
    print header
    for (i=1;i<=n;i++) {
      dominated=0
      for (j=1;j<=n;j++) if (j!=i && latency[j]<=latency[i] && energy[j]<=energy[i] && (latency[j]<latency[i] || energy[j]<energy[i])) { dominated=1; break }
      if (!dominated) print rows[i]
    }
  }' "$input" > "$output"
echo "Wrote feasible Pareto designs to $output"
