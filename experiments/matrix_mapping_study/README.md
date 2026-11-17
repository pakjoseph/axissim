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

## M6.3d serialized backing-store sensitivity (manifest v2)

The separate `manifest.v2.serialized-sensitivity.json` holds the exact M6.3d design. It inherits the six M6.3c shape/capacity cases without changing their GEMM semantics, then adds four declared settings to each paired request:

| Bandwidth assumption (decimal B/s) | Setup assumption (cycles per batch) |
|---:|---:|
| 1,000,000,000 | 0 |
| 64,000,000,000 | 0 |
| 1,000,000,000 | 1 |
| 64,000,000,000 | 1 |

This produces exactly 24 distinct requests. Preflight runs all 12 P4 candidates in planned order; only if every pilot completes successfully may `execute` run the remaining 12 P8 candidates. A failed pilot is retained in the outcome ledger and prevents P8 execution. These are fixed analytical assumptions, not values characterized from a device.

Use a new output directory for each run:

```sh
python3 experiments/run_matrix_mapping_study.py plan \
  --manifest experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json \
  --output-dir ../axissim-study-output/gemm-serial-sensitivity-001

python3 experiments/run_matrix_mapping_study.py preflight \
  --manifest experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json \
  --binary build/axissim \
  --output-dir ../axissim-study-output/gemm-serial-sensitivity-001

python3 experiments/run_matrix_mapping_study.py execute \
  --manifest experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json \
  --binary build/axissim \
  --output-dir ../axissim-study-output/gemm-serial-sensitivity-001

python3 experiments/run_matrix_mapping_study.py verify \
  --bundle ../axissim-study-output/gemm-serial-sensitivity-001

python3 experiments/run_matrix_mapping_study.py reproduce \
  --manifest experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json \
  --binary build/axissim \
  --reference-bundle ../axissim-study-output/gemm-serial-sensitivity-001 \
  --output-dir ../axissim-study-output/gemm-serial-sensitivity-replay-001
```

M6.3d keeps the same 900-second cumulative active-time budget, 30-second per-invocation timeout, 2 MiB final bundle cap, 262,144-byte staging reserve, 64 KiB request cap, and 24-candidate ceiling. The active-time record includes preflight, candidate execution, report/plot publication, and the first artifact verification pass. A run that crosses the cumulative deadline is recorded as `limit_reached`, even when all candidate calculations already finished. Preflight recomputes candidate count, request bytes, worst-case process time, and reserved storage from the fixed manifest. Each C++ result has an 8,192-byte payload cap (plus its final LF in the bundle). These bounds are planning and safety limits, not runtime guarantees. No parallel execution is used.

The v2 summary and CSV separate compute-only cycles from C++-returned serialized read-service cycles, serialized compute cycles, serialized write-service cycles, and combined serial total cycles. Per-unit cycle fields are per completed GEMM work unit; this manifest has one repeated unit. The CSV also retains the declared bandwidth/setup, candidate/request/assumption identities, status, and mapping-derived logical A/B/C bytes. The report lists all 24 rows and the preregistered H1–H5 checks, with evidence pointers into the C++ result artifacts. The two additional plots, `serialized-total-cycles.svg` and `serialized-cycle-breakdown.svg`, label all values as **modeled serial cycles**; bandwidth labels use decimal GB/s and setup labels are cycles per batch. Incomplete or failed rows remain in the ledger and do not contribute to complete-pair plots or conclusions.

The C++ `study-gemm-pair` evaluator is authoritative for compute cycles, logical bytes and transfer-batch counts, and the serialized read/compute/write/total cycle fields. Python validates identities and returned-field consistency, then groups and reports results; it does not reimplement the backing-store timing equation. Sensitivity values are part of each request, candidate, and model-assumption identity. They are not normalized to the M6.3c identities.

M6.3d's comparison remains a deterministic sensitivity exercise over logical payloads and an abstract **serialized** service assumption. It does not model compute/memory overlap, memory arbitration or queues, cache hits or residency, DRAM commands, actual physical transfers, transfer latency, memory energy or power, or physical bottlenecks. It does not model AME architectural semantics. P4/P8 remain abstract compute capacities, not equal-area, equal-power, or equal-cost hardware alternatives. Reported totals are not measured latency or hardware speedup. The two bandwidth endpoints and setup values are not confidence intervals or statistical uncertainty ranges.

The implementation acceptance E2E also supports an optional private M6.3c reference bundle. This is intentionally not required in public CI because the historical bundle is private:

```sh
python3 tests/matrix_mapping_study_e2e.py \
  --source-root . --binary build/axissim \
  --m6-3c-reference-bundle /path/to/verified/m6.3c-2026-11-16
```

That check verifies the historical bundle and requires all 24 v2 variants to preserve the six accepted M6.3c workloads, mapping IDs, work counts, operation counts, compute cycles, logical bytes, and transfer-batch counts. Sensitivity-bearing v2 identities are expected to differ.

## Test commands

```sh
ctest --test-dir build --output-on-failure -R 'matrix_mapping_study_(contract|e2e)'
python3 tests/matrix_mapping_study_tests.py --source-root . --binary build/axissim
python3 tests/matrix_mapping_study_e2e.py --source-root . --binary build/axissim
```
