#!/usr/bin/env bash
# Deterministic recommendation for the five-point illustrative power-budget study.
set -euo pipefail

input=${1:-"$(cd "$(dirname "$0")" && pwd)/power_budget_study.csv"}
[[ -f "$input" ]] || { echo "Missing study CSV: $input" >&2; exit 1; }

awk -F, '
  NR == 1 { next }
  $10 == "feasible" && ($6 + 0 < best || !seen) {
    best = $6 + 0; row = $0; seen = 1
  }
  $1 == "memory_constrained_compute" { memory_latency = $6 + 0 }
  END {
    if (!seen) { print "No feasible designs; no recommendation."; exit 1 }
    print "Recommended feasible design (minimum modeled latency):"
    print row
    if (memory_latency > best) print "Guardrail: compute reinvestment is not automatically beneficial; the memory-constrained counterexample has " memory_latency " us latency."
  }' "$input"
