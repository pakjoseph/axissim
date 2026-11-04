#!/usr/bin/env bash
set -euo pipefail
analyzer=$1
input=$(mktemp)
output=$(mktemp)
trap 'rm -f "$input" "$output"' EXIT
printf '%s\n' \
'arithmetic_scale,npu_macs,dram_bandwidth_gbps,mapping,sram_capacity_bytes,tile_m,tile_n,tile_k,latency_us,it_energy_per_frame_uj,average_it_power_w,feasible' \
'1,1,1,a,1,1,1,1,10,10,1,true' \
'1,2,1,b,1,1,1,1,9,11,1,true' \
'1,3,1,c,1,1,1,1,11,11,1,true' \
'1,4,1,d,1,1,1,1,8,8,2,false' > "$input"
"$analyzer" "$input" "$output" >/dev/null
[[ $(wc -l < "$output" | tr -d ' ') == 3 ]]
grep -q ',a,' "$output"
grep -q ',b,' "$output"
! grep -q ',c,' "$output"
! grep -q ',d,' "$output"
