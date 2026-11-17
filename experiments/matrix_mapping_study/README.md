# Paired GEMM mapping study (M6.3c)

This bounded study runs the existing C++ `study-gemm-pair` evaluator for six shape-only GEMM requests: three small shapes at two matched abstract arithmetic capacities (P4 and P8). Python 3.10+ standard library code handles manifest validation, deterministic request generation, process management, artifact integrity, and presentation. The C++ evaluator remains authoritative for mapping status, analytical compute cycles, operation counts, and logical payload bytes.

## Reproduce

Build AxisSim as usual, then choose a new output directory outside checked-in reference/fixture directories. The example writes beside the checkout; the directory must not already exist.

```sh
python3 experiments/run_matrix_mapping_study.py plan \
  --manifest experiments/matrix_mapping_study/manifest.v1.json \
  --output-dir ../axissim-study-output/gemm-run-001

python3 experiments/run_matrix_mapping_study.py preflight \
  --manifest experiments/matrix_mapping_study/manifest.v1.json \
  --binary build/axissim \
  --output-dir ../axissim-study-output/gemm-run-001

python3 experiments/run_matrix_mapping_study.py execute \
  --manifest experiments/matrix_mapping_study/manifest.v1.json \
  --binary build/axissim \
  --output-dir ../axissim-study-output/gemm-run-001

python3 experiments/run_matrix_mapping_study.py verify \
  --bundle ../axissim-study-output/gemm-run-001

python3 experiments/run_matrix_mapping_study.py reproduce \
  --manifest experiments/matrix_mapping_study/manifest.v1.json \
  --binary build/axissim \
  --reference-bundle ../axissim-study-output/gemm-run-001 \
  --output-dir ../axissim-study-output/gemm-replay-001
```

Each output root must be new. A failed phase preserves its root for diagnosis; use a different path for another attempt. The manifest is strict v1 and fixes the six-case set, mappings, measurement protocol, and operational limits. The three P4 requests run as preflight pilots; execute reuses those outputs and runs the three P8 cases. A successful initial study therefore invokes C++ six times total.

`reproduce` also verifies and compares a valid terminal preflight-failure bundle, such as a repeatable pilot failure or preflight limit. It exits successfully only when the canonical artifacts match; host diagnostics remain separately integrity-checked. If a phase fails before it can publish a verifiable bundle, reproduction fails instead of claiming a match.

## Bundle contents

- `manifest.json`, `plan.json`, and `preflight.json` record the fixed study and candidate order.
- `provenance.json` records manifest, runner, executable, observed checkout, and request identities. The observed checkout is not proof of the binary's build source; C++ compiler/build fields remain `unknown` when reported that way.
- `requests/` and `results/` contain canonical C++ contract records.
- `runner-ledger.jsonl`, `summary.json`, and `summary.csv` retain every planned row and distinguish complete, duplicate, partial, invalid, failed, timeout, process, identity, output-limit, and unstarted outcomes.
- `report.md` gives the preregistered hypothesis outcomes, row details, exact result JSON pointers, and limitations.
- `plots/compute-cycles.svg` and `plots/logical-payload-bytes.svg` visualize only complete paired results.
- `canonical-artifact-manifest.json` hashes deterministic artifacts. `bundle-index.json` separately hashes that manifest and host diagnostics.
- `host-diagnostics.jsonl` keeps monotonic phase/process durations and stdout/stderr byte counts and hashes; it does not retain raw stream text or wall-clock timestamps.

`preflight.json` reports the conservative evaluator process bound (six candidates × 30 seconds = 180 seconds for this study) and reserved artifact peak. The 15-minute active evaluator budget covers preflight and execution activity, excluding time between commands. These are operational bounds, not completion-time guarantees. Host elapsed times are not simulator latency.

Subprocess exit status is serialized as a nonnegative canonical integer: native nonnegative exit codes are unchanged; POSIX signal termination uses the conventional `128 + signal number` status. If a process could not be launched, the unavailable `process_exit_code` field is omitted.

## Interpretation limits

P4/P8 are abstract MAC-slot-per-cycle assumptions, not equal-area, equal-power, or equal-cost designs. The table's cycle values are C++ analytical compute cycles at the declared 1 GHz model clock; any ratio derived from them is a modeled quantity, not a measured speedup. A/B/C byte values are mapping-derived logical payload only. They are not physical memory transfers, SRAM actions, DRAM commands, or energy. The shape classes also use different matrix tile geometries, so cross-shape changes do not isolate shape alone.

The workload is shape-only: AxisSim counts the declared mathematical work but does not load operands or calculate output matrices. The study does not model physical power, energy, area, AME instructions or compliance, CPU dispatch, overlap, caches, shared-memory arbitration, or hardware variability. It supports no physical bottleneck, energy-efficiency, or silicon-design recommendation. The optional serialized-bandwidth sensitivity is not enabled in this manifest.

## Test commands

```sh
ctest --test-dir build --output-on-failure -R 'matrix_mapping_study_(contract|e2e)'
python3 tests/matrix_mapping_study_tests.py --source-root . --binary build/axissim
python3 tests/matrix_mapping_study_e2e.py --source-root . --binary build/axissim
```
