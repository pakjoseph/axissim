# Budget-Constrained Study Planning

This directory contains the checked-in planning manifest and the selected, bounded study manifest. The runner currently supports **planning and preflight only**. It does not execute the 168 selected architecture candidates or create Pareto results; those steps belong to the next reviewed milestone.

## Requirements and commands

Planning uses Python 3.10 or later and Python's standard library only. Python is not needed for normal AxisSim CLI or desktop use. Build the C++ simulator first:

```sh
cmake -S . -B build -DAXISSIM_BUILD_GUI=OFF
cmake --build build
python3 experiments/run_budget_constrained_study.py plan \
  --manifest experiments/budget_constrained_study/planning_manifest.json \
  --binary build/axissim \
  --report build/study-plan-report.json
python3 experiments/run_budget_constrained_study.py preflight \
  --manifest experiments/budget_constrained_study/manifest.json \
  --binary build/axissim \
  --plan-report build/study-plan-report.json \
  --report build/study-final-preflight.json
ctest --test-dir build --output-on-failure -R study_manifest_preflight_contract
```

`plan` runs exactly one ordinary `axissim run` baseline pilot for Physical AI, MNIST-12, and GEMM-heavy. These pilots measure host execution time and artifact sizes only. `plan` enumerates the full checked-in sensitivity pools and performs configuration/profile validation; it does not run the enumerated candidates. The full planning pool contains 372 deduplicated rows, including eight SRAM/tile and ten weight-capacity combinations rejected by the simulator's existing fit validation. Those rejected rows remain visible in the report.

`preflight` expands the explicit rows in `manifest.json`, verifies the source hashes and build identity, applies every declared cap, validates each selected configuration and profile, and writes the exact candidate list. The current manifest selects 168 candidates: 58 Physical AI, 56 MNIST-12, and 54 GEMM-heavy, with periodic and trace-arrival strata kept separate. Its `max_candidates` limit is exactly 168. Lowering the cap or adding rows must reject preflight before any per-candidate configuration validation begins.

For the checked-in selected rows, preflight estimates 14,280 task events at the busiest candidate, 3,627,008 bytes for its task trace, and 188,104,704 bytes if every candidate's trace is retained. It counts 1,741,552 bytes for canonical M3 v1 request envelopes exactly, reserving one no-trace request and one possible full-trace replay request per candidate. Total selected-study output reserve is 201,130,736 bytes for requests, result records, traces, and summary reserve. The explicit limits are 20,000 events per candidate, 4 MiB per candidate trace, 512 MiB total retained traces, 65,536 bytes per result, and 1 GiB total study output. The model-time, drain, backlog, and wall-time limits are also explicit in `manifest.json`.

The plan and preflight reports contain host-side time and storage estimates. RVV chunking is reflected in task-count and trace-size estimates. Task JSONL trace size uses a conservative 2,048-byte per-record allowance plus a per-candidate header; no task JSONL trace is collected by the plan pilots, so this remains an estimate. Final preflight reserves each selected candidate's exact canonical M3 request bytes (without a trailing newline), a possible trace-replay request, a result record up to the declared per-candidate cap, and conservative trace/summary space. The full planning-pool storage estimate counts 1,162,006 planning-descriptor bytes and labels that candidate request artifacts are not included there, because the planning manifest has no finalized operational limits. The 372-candidate full planning pool (398 rows before deduplication) estimates 330,080,256 trace bytes and 347,017,590 output bytes excluding unfinalized candidate requests, including all 18 invalid fit rows as if they had been selected.

## Contracts and ownership

- `planning_manifest.json` uses `axissim.study-planning-manifest.v1` and records the complete checked-in level pools and interactions. It has no operational limits and cannot silently remove levels.
- `manifest.json` uses `axissim.budget-constrained-study.v1`. It explicitly lists selected candidate rows, all nine positive execution limits, per-factor rationale, input hashes, and trace policy.
- `study-plan-report.v1` records the exact full expansion, pilot identities, configuration validation outcomes, and host diagnostics.
- `study-preflight-report.v1` records the exact selected expansion, cap-by-cap pass/reject status, profile/config validation outcomes, and `candidate_simulations_launched: 0`.
- Candidate IDs and configuration/workload/protocol identities use AxisSim Study Canonical JSON v1 and SHA-256. Duplicate JSON keys, non-finite numbers, unsupported manifest fields, stale input hashes, and unlisted factor values are rejected.
- C++ remains the sole owner of simulated execution, timing, dynamic/static energy, average modeled IT power, and numeric budget status. Python does not reproduce those equations.
- Preflight failures use exit code 2 and a machine-readable `structured_status: rejected` report; success requires both exit code 0 and `structured_status: ready`. The M4 suite also checks that the C++ candidate evaluator's process exit and JSON execution status agree for censored and result-size-fallback records.
- Candidate execution, result aggregation, failure isolation across an entire study, and Pareto reporting are not part of this milestone; they remain deferred to M5.

## Power scope

The fixed modeled IT-power budget remains 0.23 W. MNIST-12 receives a study-only overlay containing the documented coefficients from `configs/power_budget/manifest.conf`; `configs/workloads/mnist12_profiled.conf` retains its legacy zero-default power fields. The overlay itself does not add SRAM access energy or bandwidth-scaled DRAM-interface power.

The cost coverage declared in a manifest is a planning expectation. C++ must resolve actual factor coverage and the budget result when the later candidate-evaluation milestone runs. A numeric pass may only be described as **provisionally feasible under current modeled power terms**. It is not a claim of physically feasible silicon. DRAM-interface bandwidth scaling, SRAM access energy, and resident-weight SRAM costs remain missing or partial, so changes relying on them remain exploratory.

Arm64 and x86 measurements elsewhere in AxisSim are host-validation evidence. They are not measurements of a hypothetical RISC-V/AME target and do not calibrate the 0.23 W model.
