#!/usr/bin/env bash
# Compare named mapping policies over the same public static MNIST-12 profile.
# Results are analytical model outputs, not measured compiler or silicon data.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$root/build/axissim"}
output=${2:-"$root/experiments/mnist12_mapping_policy_study.csv"}
temporary=$(mktemp)
trap 'rm -f "$temporary"' EXIT

mkdir -p "$(dirname "$output")"
echo 'policy,mapping_plan,mean_latency_us,it_energy_per_frame_uj,dram_bytes,first_frame_bytes,steady_state_bytes,source_storage_precision,target_storage_precision,precision_traffic_bytes_avoided_per_frame,mapped_npu_arithmetic_energy_scale,fusion_count,fused_intermediate_bytes_avoided_per_frame,persistent_weight_operator_count,persistent_weight_bytes_avoided_per_steady_frame' > "$output"

for policy in baseline fused persistent fused_persistent fp16_storage; do
  case "$policy" in
    baseline) config="$root/configs/workloads/mnist12_profiled.conf" ;;
    fused) config="$root/configs/workloads/mnist12_plus_relu_fused.conf" ;;
    persistent) config="$root/configs/workloads/mnist12_persistent_weights.conf" ;;
    fused_persistent) config="$root/configs/workloads/mnist12_fused_persistent_weights.conf" ;;
    fp16_storage) config="$root/configs/workloads/mnist12_fp16_storage.conf" ;;
  esac
  "$binary" run "$config" --json "$temporary" >/dev/null
  jq -r --arg policy "$policy" '[
    $policy,
    .mapping_plan,
    .latency.mean_us,
    .energy.it_per_frame_uj,
    .traffic.dram_bytes,
    .profile_transfer_ledger.first_frame_bytes,
    .profile_transfer_ledger.steady_state_bytes,
    .mapping_effects.source_storage_precision,
    .mapping_effects.target_storage_precision,
    .mapping_effects.precision_traffic_bytes_avoided_per_frame,
    .mapping_effects.mapped_npu_arithmetic_energy_scale,
    .mapping_effects.fusion_count,
    .mapping_effects.fused_intermediate_bytes_avoided_per_frame,
    .mapping_effects.persistent_weight_operator_count,
    .mapping_effects.persistent_weight_bytes_avoided_per_steady_frame
  ] | @csv' "$temporary" >> "$output"
done

echo "Wrote five named mapping-policy points to $output"
