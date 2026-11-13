# Budget-Constrained Architecture Co-Design Study

This directory contains the checked-in planning manifest and the selected, bounded study manifest. The runner supports planning, preflight, bounded candidate execution, result collection, Pareto analysis, and deterministic reproduction. It uses Python 3.10+ and the standard library; Python is not needed for normal AxisSim CLI or desktop use.

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
python3 experiments/run_budget_constrained_study.py execute \
  --manifest experiments/budget_constrained_study/manifest.json \
  --binary build/axissim \
  --plan-report build/study-plan-report.json \
  --preflight-report build/study-final-preflight.json \
  --output-dir build/study-pilot --pilot
python3 experiments/run_budget_constrained_study.py execute \
  --manifest experiments/budget_constrained_study/manifest.json \
  --binary build/axissim \
  --plan-report build/study-plan-report.json \
  --preflight-report build/study-final-preflight.json \
  --output-dir build/study-results
python3 experiments/run_budget_constrained_study.py reproduce \
  --manifest experiments/budget_constrained_study/manifest.json \
  --binary build/axissim \
  --plan-report build/study-plan-report.json \
  --preflight-report build/study-final-preflight.json \
  --reference-study build/study-results \
  --output-dir build/study-reproduction
ctest --test-dir build --output-on-failure -R "study_(manifest_preflight|m5_execution_pareto)_contract"
```

Each output directory must be new or empty. Run the three-workload pilot first, review its host resource diagnostics, then run the full study. `reproduce` re-evaluates the same candidates and compares the canonical artifacts. The study summary, candidate requests/results, per-workload reports, Pareto CSVs, and trace index are written under the selected output directory. Host runtime and platform diagnostics are kept separately from the canonical result artifacts.

`plan` runs exactly one ordinary `axissim run` baseline pilot for Physical AI, MNIST-12, and GEMM-heavy. These pilots measure host execution time and artifact sizes only. `plan` enumerates the full checked-in sensitivity pools and performs configuration/profile validation; it does not run the enumerated candidates. The full planning pool contains 372 deduplicated rows, including eight SRAM/tile and ten weight-capacity combinations rejected by the simulator's existing fit validation. Those rejected rows remain visible in the report.

`preflight` expands the explicit rows in `manifest.json`, verifies the source hashes and build identity, applies every declared cap, validates each selected configuration and profile, and writes the exact candidate list. The current manifest selects 168 candidates: 58 Physical AI, 56 MNIST-12, and 54 GEMM-heavy, with periodic and trace-arrival strata kept separate. Its `max_candidates` limit is exactly 168. Lowering the cap or adding rows must reject preflight before any per-candidate configuration validation begins.

For the checked-in selected rows, preflight estimates 14,280 task events at the busiest candidate, 3,627,008 bytes for its task trace, and 188,104,704 bytes if every candidate's trace is retained. It counts 1,741,552 bytes for canonical M3 v1 request envelopes exactly, reserving one no-trace request and one possible full-trace replay request per candidate. Total selected-study output reserve is 219,079,408 bytes for requests, result records, traces, and summary reserve. The explicit limits are 20,000 events per candidate, 4 MiB per candidate trace, 512 MiB total retained traces, 65,536 bytes per result, and 1 GiB total study output. The model-time, drain, backlog, and per-candidate wall-time limits are also explicit in `manifest.json`.

The plan and preflight reports contain host-side time and storage estimates. RVV chunking is reflected in task-count and trace-size estimates. Task JSONL trace size uses a conservative 2,048-byte per-record allowance plus a per-candidate header; no task JSONL trace is collected by the plan pilots, so this remains an estimate. Final preflight reserves each selected candidate's exact canonical M3 request bytes (without a trailing newline), a possible trace-replay request, a result record up to the declared per-candidate cap, and conservative trace/summary space. The full planning-pool storage estimate counts 1,162,006 planning-descriptor bytes and labels that candidate request artifacts are not included there, because the planning manifest has no finalized operational limits. The 372-candidate full planning pool (398 rows before deduplication) estimates 330,080,256 trace bytes and 347,017,590 output bytes excluding unfinalized candidate requests, including all 18 invalid fit rows as if they had been selected.

## Contracts and ownership

- `planning_manifest.json` uses `axissim.study-planning-manifest.v1` and records the complete checked-in level pools and interactions. It has no operational limits and cannot silently remove levels.
- `manifest.json` uses `axissim.budget-constrained-study.v1`. It explicitly lists selected candidate rows, all nine positive execution limits, per-factor rationale, input hashes, and trace policy.
- `study-plan-report.v1` records the exact full expansion, pilot identities, configuration validation outcomes, and host diagnostics.
- `study-preflight-report.v1` records the exact selected expansion, cap-by-cap pass/reject status, profile/config validation outcomes, and `candidate_simulations_launched: 0`.
- Candidate IDs and configuration/workload/protocol identities use AxisSim Study Canonical JSON v1 and SHA-256. Duplicate JSON keys, non-finite numbers, unsupported manifest fields, stale input hashes, and unlisted factor values are rejected.
- C++ remains the sole owner of simulated execution, timing, dynamic/static energy, average modeled IT power, and numeric budget status. Python does not reproduce those equations.
- Preflight failures use exit code 2 and a machine-readable `structured_status: rejected` report; success requires both exit code 0 and `structured_status: ready`. The M4 suite also checks that the C++ candidate evaluator's process exit and JSON execution status agree for censored and result-size-fallback records.
- Candidate process failures, timeouts, censored results, and nonzero exits are retained as structured failure records and excluded from complete-result Pareto frontiers. A failed candidate does not erase other candidates' results.
- Pareto comparisons are separate by workload, release stratum, measurement/model identities, objective applicability, service state, and power-cost coverage. Sensitivity-only changes to model assumptions are reported but are not architectural Pareto recommendations.

## M5 execution record

On the recorded macOS arm64 run, the approved selection completed: 58 Physical AI, 56 MNIST-12, and 54 GEMM-heavy candidates; 168/168 completed, with no invalid, failed, censored, or skipped execution records. All 168 were under the existing numeric 0.23 W modeled average IT-power budget. This is a numeric model result only; resource costs remain incomplete, so the pass does not establish physical feasibility.

Planning contains 398 rows before deduplication and 372 unique candidates after removing 26 duplicate rows. The 18 invalid SRAM-fit combinations are retained in planning diagnostics but were not executed or included in Pareto analysis. The full run produced 50 comparison groups, 36 computed frontiers, 12 sensitivity-only groups, and two non-applicable groups with no frontier. Frontiers remain local to their exact comparison groups; exploratory results that change partly or unpriced resources are not treated as adequately costed recommendations.

The preflight projects **candidate evaluator process time**, not end-to-end study wall time: it sums baseline-pilot time by candidate and applies a 2× allowance for full trace replay. In the final recorded run, the projection was 2.57 seconds; actual first-pass evaluator time was 3.53 seconds and trace-replay evaluator time was 2.15 seconds, for 5.68 seconds total candidate process time (about 2.21× the projection). End-to-end study wall time was 17.66 seconds because it also includes orchestration, report/Pareto generation, and artifact hashing; that is not the same quantity as the preflight projection. The corrected diagnostics report candidate and full-study timings separately and write each candidate's first-pass record once. This host-time gap does not affect simulated metrics and did not violate the per-candidate timeout or output/disk limits.

A runtime-estimator correction is warranted before treating preflight as a duration commitment, but the numerical correction is deferred: one full run on one host is not enough to select a portable calibration factor. The next estimate should use a trace-enabled pilot and report candidate subprocess time separately from Python orchestration/reporting. The current estimate remains a rough host diagnostic. Actual canonical artifacts occupied 29.04 MB, below the 219.08 MB preflight reserve and the 1 GiB study cap; retained traces occupied 22.05 MB across 88 candidate trace files, with no replay failures. The pilot completed all three workload families and used about 0.82 MB. The deterministic reproduction check found zero canonical artifact differences.

### Energy metric scope

The study's `energy_per_completed_work_unit_j` comes from the C++ measurement contract. Its numerator is total modeled IT energy accumulated over the measurement interval; `energy_work_unit_denominator` in each candidate result gives its denominator (completed frames plus control work units when present). For a finite workload, energy covers the makespan and the denominator includes units completed after the finite run drains. For a sustained workload, energy is integrated over `[t0,t1)` and divided by work units completed inside that same window; the latency/deadline cohort released during the window is tracked separately.

The three selected study baseline profiles use finite, frame-only workloads: each completes 30 frame units and zero control units. Their study denominator is therefore 30 frames, and the reported metric can be described as joules per completed frame. The desktop overview and legacy CLI also report the schema-1.2 whole-run `IT energy / frame`: total IT energy over the makespan divided by completed vision frames. The denominator is a frame count in both cases, but the energy values are not necessarily equal: the study applies its declared power-budget configuration/overlay, while the GUI uses the selected scenario configuration. For example, the Physical AI study baseline includes about 4.177 mJ/frame of modeled static energy, in addition to about 0.108 mJ/frame of dynamic energy; a GUI run with different static assumptions will report a different IT-energy value. Control-task energy may also contribute to a whole-run numerator while its legacy denominator remains vision frames. Compare numeric results only when workload inputs, model and power assumptions, and measurement interval match. The GUI energy card and breakdown tooltips explain these scopes.

The local run used Python 3.14.2. CI now has Python 3.10 and 3.12 runner-contract jobs, but they were not executed from this uncommitted worktree; compatibility with those interpreters remains pending remote CI verification.

The SRAM-capacity/tile study is a sparse combined-factor sample. Tile M, N, and K dimensions move together in several rows, and the capacity/tile combinations are not fully crossed. Its results describe only the tested configurations; they cannot isolate independent capacity and tile effects or establish a complete interaction. Pareto membership retains candidates tied within the declared objective tolerance; the reports identify these as metric-equivalent under the tested objectives. A `current_model_covered` label means the swept factors' incremental costs are represented, not that all system power terms are fully characterized.

Per-workload reports summarize the largest one-factor latency range and the tested NPU MAC-count versus DRAM-bandwidth spans. These are model sensitivity diagnostics, not stall attribution or physical bottleneck measurements; the reports do not claim an observed physical bottleneck migration.

## Power scope

The fixed modeled IT-power budget remains 0.23 W. MNIST-12 receives a study-only overlay containing the documented coefficients from `configs/power_budget/manifest.conf`; `configs/workloads/mnist12_profiled.conf` retains its legacy zero-default power fields. The overlay itself does not add SRAM access energy or bandwidth-scaled DRAM-interface power.

The cost coverage declared in a manifest is a planning expectation; the C++ candidate result reports the resolved coverage and budget status. A numeric pass may only be described as **provisionally feasible under current modeled power terms**. It is not a claim of physically feasible silicon. DRAM-interface bandwidth scaling, SRAM access energy, and resident-weight SRAM costs remain missing or partial, so changes relying on them remain exploratory.

Arm64 and x86 measurements elsewhere in AxisSim are host-validation evidence. They are not measurements of a hypothetical RISC-V/AME target and do not calibrate the 0.23 W model.
