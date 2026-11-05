#!/usr/bin/env bash
set -euo pipefail
binary=$1
baseline=$2
periodic_trace=$3
burst_trace=$4
regular=$(mktemp)
trace=$(mktemp)
burst=$(mktemp)
trap 'rm -f "$regular" "$trace" "$burst"' EXIT
"$binary" run "$baseline" --json "$regular" >/dev/null
"$binary" run "$periodic_trace" --json "$trace" >/dev/null
"$binary" run "$burst_trace" --json "$burst" >/dev/null
jq -e --slurpfile regular "$regular" --slurpfile trace "$trace" '
  $regular[0].latency == $trace[0].latency and
  $regular[0].deadlines == $trace[0].deadlines and
  $regular[0].traffic == $trace[0].traffic and
  $trace[0].arrival_mode == "trace" and
  $trace[0].qos.vision_queue_delay_max_us == 0
' "$trace" >/dev/null
jq -e '
  .arrival_mode == "trace" and
  .qos.vision_queue_delay_max_us > 0 and
  .qos.output_interval_jitter_max_us > 0 and
  .qos.max_pending_tasks > 0
' "$burst" >/dev/null
