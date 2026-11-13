#!/usr/bin/env bash
set -euo pipefail
analyzer=$1
fixture=$2
input=$(mktemp)
output=$(mktemp)
trap 'rm -f "$input" "$output"' EXIT
cp "$fixture" "$input"
"$analyzer" "$input" "$output" >/dev/null
[[ $(wc -l < "$output" | tr -d ' ') == 3 ]]
grep -q ',a,' "$output"
grep -q ',b,' "$output"
! grep -q ',c,' "$output"
! grep -q ',d,' "$output"
