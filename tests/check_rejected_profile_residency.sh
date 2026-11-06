#!/usr/bin/env bash
set -euo pipefail

binary=$1
config=$2
output=$(mktemp)
trap 'rm -f "$output"' EXIT

if "$binary" run "$config" >"$output" 2>&1; then
  echo "expected persistent-weight mapping to be rejected" >&2
  exit 1
fi

grep -F "Mapping-plan persistent weights require 13600 bytes of weight SRAM; configured capacity is 8192" "$output" >/dev/null
