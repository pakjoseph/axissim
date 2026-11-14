# Power Accounting Verification and Traceability

This report distinguishes **verification** of current equations from **validation** against target hardware and **calibration** of coefficients. M6.1 verifies the implementation against independently derived model arithmetic; it does not establish physical accuracy.

## Source-to-test traceability

| Requirement / behavior | Implementation owner | Verification evidence |
|---|---|---|
| Vector, NPU, DRAM, and DMA dynamic categories and additive total | `src/simulation.hpp`: task creation, `runtime_task_energy()`, `accumulate_runtime_energy()` | `core_model_api_contract`: `test_dynamic_energy_components_against_independent_activity_counts` uses a hand-counted one-frame physical workload under both scheduler-ordering policies. |
| Static power term sum, pJ/µs-to-W conversion, IT energy, PUE, and budget boundary | `src/metrics.cpp`: `modeled_static_power_w()`, `model_accounting()` | `core_model_api_contract`: `test_accounting_matches_independent_unit_derivation` derives a synthetic 0.625 W static total, 0.25 W dynamic component, 0.875 W IT total, and checks at-limit/next-representable-below behavior. `test_accounting_energy_and_power_budget_boundaries` retains the 0.23 W fixture arithmetic and disabled-budget behavior. |
| Each static term and configured resource-capacity effects | `src/metrics.cpp`: `modeled_static_power_w()` | `core_model_api_contract`: `test_static_power_terms_are_independently_accounted` checks base, NPU, RVV, main SRAM, and DRAM-interface terms individually and in sum; the independent aggregate case also doubles configured NPU and main-SRAM capacity. `larger_npu_has_explicit_capacity_power` remains a CLI regression. |
| Isolated energy sensitivity and unpriced-resource behavior | `src/simulation.hpp`, `src/metrics.cpp` | `core_model_api_contract`: `test_dynamic_energy_sensitivities_are_isolated` varies vector work, GEMM MAC count, NPU scale, voltage ratio, and input bytes while independently checking affected and unaffected energy categories; it confirms DRAM-bandwidth and resident-weight capacity do not add a term to the current static equation. |
| Static versus dynamic energy aggregation | `src/metrics.cpp`, `src/simulation.hpp` | Independent hand-computed core tests above; `exact_power_accounting` checks CLI JSON dynamic category sum, dynamic/frame, static/frame, IT/frame, PUE, and configured static terms. |
| Finite makespan and finite energy per completed unit | `src/study_measurements.hpp` | `study_measurement_contract`: `test_finite_makespan_and_static_energy` plus new `test_finite_multiunit_energy_denominator` checks two completed units, known dynamic/static energy, average power, and per-unit denominator. |
| Sustained `[t0,t1)` energy, throughput, cohort, and drain isolation | `src/study_measurements.hpp` | `study_measurement_contract`: `test_sustained_window_cohort_backlog_and_drain`, including window-energy per completed unit; `test_sustained_scheduler_cutoff_and_frozen_energy` verifies frozen window metrics across different drains. |
| Adjacent-window dynamic/static energy conservation and boundary cases | `src/study_measurements.hpp` | `study_measurement_contract`: `test_overlap_and_energy_partition_boundaries`, `test_static_energy_adjacent_windows_conserve`, boundary-crossing, zero-duration, and exact-`t1` cases. |
| Coverage and provisional-feasibility contract | `src/study_contract.cpp` | `study_candidate_cli_contract`: verifies candidate coverage schema and representative NPU/DRAM/SRAM classifications; `test_budget_boundary_uses_reported_cpp_power_and_coverage` verifies the C++ budget output and its separate coverage. Python M5 contract tests verify grouping/eligibility for partial or missing terms. |
| Human inventory remains aligned with machine-readable factors and C++ coverage terms | `experiments/budget_constrained_study/manifest.json`, `src/study_contract.cpp`, `docs/power-cost-coverage.md` | `power_cost_coverage_documentation_contract` compares every manifest factor to its exact represented/omitted C++ terms and applicability, and checks status rules, equation fragments, defaults, and 0.23 W reference inputs. |
| Legacy per-frame dynamic/IT distinction | `src/cli/main.cpp`, `src/gui/main.cpp` | `exact_power_accounting` plus M5 baseline parity; GUI KPI/chart tooltips state the IT-energy numerator and frame denominator. |
| 0.23 W and historical MNIST defaults | configuration and study overlay in `src/config.cpp`, `src/study_contract.cpp` | `mnist12_study_power_overlay_preserves_legacy_config` and M0/M5 baseline parity; no baseline config or fixture is changed by M6.1. |
| `legacy_implicit` and `deterministic_v1` accounting paths | `src/config.hpp`, `src/simulation.hpp`, `src/scheduler_ordering.hpp` | Dynamic component test runs both policies; `scheduler_ordering_cpp_contract`, `scheduler_ordering_deterministic_contract`, and platform-specific legacy parity/repeatability tests cover scheduler semantics. |
| Accepted M5 candidate outputs and retained traces | C++ evaluator and Python runner | `tools/verify_m5_study_replay.py` checks the pinned preserved reference identity and artifact hashes, then compares all matched requests, result records, and retained task traces after removing only build-specific candidate/simulator identity. `m5_replay_verifier_contract` checks artifact path containment. `study_m5_execution_pareto_contract` remains the runner's repeatability contract; it is not itself the 168-candidate M5 replay. |

## Independent expected-value cases

### Aggregate model accounting

The new core accounting case uses exact binary-friendly values independent of `model_accounting()`:

```text
P_static = 0.125 + 4×0.03125 + 4×0.03125 + 8 KiB×0.015625 + 0.125
         = 0.625 W
E_dynamic = 2,500,000 pJ
duration  = 10 µs
P_dynamic = 2,500,000 / 10 / 1,000,000 = 0.25 W
E_static = 0.625 × 10 × 1,000,000 = 6,250,000 pJ
E_IT = 8,750,000 pJ
P_IT_avg = 0.875 W
P_facility = 1.5 × 0.875 = 1.3125 W
```

It checks the exact budget boundary and one representable value below it, zero configured budget, zero activity, and zero duration. These are model-equation checks, not a claim that the synthetic coefficients describe hardware.

### Dynamic activity categories

The one-frame test independently counts 22 vector operations and 24 NPU MACs, then applies the explicit test configuration coefficients. It independently derives DRAM and DMA category costs from the scheduler's reported transferred-byte count, and verifies their sum against total dynamic energy. Both scheduler-ordering policies execute the same workload contract. This checks energy arithmetic without requiring their event traces to match.

### Measurement denominators

The finite observer test attributes 2 µJ and 6 µJ to two complete work units over a 10 µs makespan. With 0.2 W configured static power, static energy is 2 µJ, IT energy is 10 µJ, average IT power is 1 W, and the required two-unit denominator yields 5 µJ/unit.

The sustained observer case checks the already specified half-open interval, in-window completion count, and per-completed-unit denominator separately from the release-cohort completion count. The observation-window energy and duration remain frozen during post-window draining.

## Evidence classes and limitations

- **Verified:** code implements the equations stated in this document for tested configurations and boundaries.
- **Sensitivity:** deterministic changes under explicit parameter variation; not a statistical interval.
- **Calibrated:** none of the configured power coefficients is established as calibrated by these tests.
- **Physically validated:** no claim is made that the hypothetical target power values match silicon. Existing Ara checks are timing/compute-envelope evidence only and do not validate this power model.

The M3 coverage classification is factor-relative and may call a factor `complete` when its listed terms have positive coefficients. That is not whole-chip completeness: SRAM access energy, resident-weight SRAM costs, bandwidth-dependent DRAM interface costs, scheduler energy, separate idle/leakage terms, and frequency/voltage power coupling remain missing or partial. Numeric budget pass, coverage, measurement completion, and recommendation eligibility must remain separate.

The task observer distributes each dispatch-attributed dynamic-energy amount uniformly across the task's modeled execution interval. This supports deterministic interval accounting and conservation tests. It is not measured activity, instantaneous power, or a valid peak-power waveform.

## Regression and artifact preservation

The acceptance gates for this branch are:

1. Full locally available CTest, including GUI checks.
2. Python study manifest, candidate API, M5 execution/Pareto, and other study contract suites.
3. Exact M0 historical baseline reproduction on macOS ARM; same-platform repeatability on platforms with legacy implicit tie ordering differences.
4. Deterministic-v1 reference and scheduler ordering checks.
5. M5 replay compared by `python3 tools/verify_m5_study_replay.py --reference <preserved-m5-directory> --replay <new-replay-directory>`: the verifier pins the accepted manifest identity, summary SHA-256, and simulator identity; verifies canonical artifact hashes; then removes only `candidate_id` and `simulator_build_identity` from matching request/result/trace records and compares every other field exactly. The accepted artifact directory and fixture must remain unchanged.
6. Diff checks proving no baseline fixture, accepted study artifact, model coefficient, scheduler semantics, or public JSON contract changed.

Passing these tests verifies preservation of documented model behavior; it does not repair the known missing physical costs or establish calibration.

## M6.1 execution evidence

This section records the local M6.1 acceptance run. The branch is based on accepted M6.0 commit `7cd3e8e`; its production simulator source, model coefficients, legacy result schema, and scheduler semantics are unchanged. The accepted M0 fixture SHA-256 is `a9a0bcacb04bca7b864c82d9ea9e8cbdf04f45b54b1422a6b6796e7595ec52cb`.

The accepted M5 study directory is local ignored build output at `build/study-m5-final-accepted`, not a tracked source artifact. The accepted bundle has manifest identity `52cb0f4be4afdce42bab3ef8e6dec036a7eefa6b6429319306ba5feaa7a92256`, summary SHA-256 `b632b2b53258501c7958b353fc174afefca3923f8618e0f0a00ca71b96f12b3e`, and a 555-entry artifact hash inventory. A clean machine needs an archived copy of that bundle (or a separately generated reference at M5) to repeat the full result/trace comparison.

The local M5 replay result is recorded in ignored `build/m6_1_m5_replay_verification.json`. The verifier pins the accepted manifest identity `52cb0f4be4afdce42bab3ef8e6dec036a7eefa6b6429319306ba5feaa7a92256`, summary SHA-256 `b632b2b53258501c7958b353fc174afefca3923f8618e0f0a00ca71b96f12b3e`, and original simulator identity `6be8f336e12c75eca1dba46d61c00302a5d4b4881e8ce10315dd48c6a659107e`; it then verified both the preserved bundle and the replay's own canonical inventory at 555/555 hashes. It matched 168/168 candidate requests; matched all 168 result records after removing only candidate and simulator-build identities; and matched all 88 retained traces (58,080 task records) after the same two identity fields were removed. Config, workload, model, measurement, factor, numerical, status, and event fields were compared exactly. The replay simulator identity was `4b134134f4be8073c47a203c72a4061eac702839551be845c2c19db828cbf9ae`.

The runner's raw canonical comparison still reports provenance-bearing artifact differences because candidate IDs, paths, and embedded build identities change with the executable. This does not contradict the explicit identity-normalized result and trace comparison above. The normalization is narrow: all non-identity request, result, and task-event fields must match exactly; no numerical tolerances are applied.

Local verification ran on macOS ARM with Python 3.14.2. `cmake --build build` succeeded. The full CTest suite, including GUI and Python study tests, passed 68/68. The M0 exact historical comparison passed locally. Python 3.10 and 3.12 were verified in the [M6.1 GitHub Actions run](https://github.com/pakjoseph/axissim/actions/runs/43639458851): [Python 3.10 contracts](https://github.com/pakjoseph/axissim/actions/runs/43639458851/job/132583505810), [Python 3.12 contracts](https://github.com/pakjoseph/axissim/actions/runs/43639458851/job/132583505559), [Ubuntu desktop/full regression](https://github.com/pakjoseph/axissim/actions/runs/43639458851/job/132583505860), [Windows desktop/portability](https://github.com/pakjoseph/axissim/actions/runs/43639458851/job/132583505718), and [macOS core](https://github.com/pakjoseph/axissim/actions/runs/43639458851/job/132583505887) all passed. The macOS workflow's M0 exact historical check and deterministic scheduler checks passed; Ubuntu and Windows ran their configured regression/portability gates. This CI validates the submitted software and test matrix; the complete 168-candidate M5 replay remains a local verification because its accepted reference bundle is ignored build output and is not uploaded to CI.
