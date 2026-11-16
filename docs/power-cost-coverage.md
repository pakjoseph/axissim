# Power and Energy Cost Coverage

AxisSim reports **scenario-model outputs**, not measured chip power. A configured coefficient is an input to a declared model; it is not a characterization result unless the scenario records a suitable external source and provenance.

This document describes the current M6.1 model only. It does not add cost terms or revise the historical 0.23 W study. The source-to-test map and verification limits are in [Power accounting verification](power-accounting-verification.md).

## Accounting owners

- `Config` defaults and units are declared in [`src/config.hpp`](../src/config.hpp); parsing and range checks are in [`src/config.cpp`](../src/config.cpp).
- Workload activity and task energy are created in [`src/simulation.hpp`](../src/simulation.hpp). `runtime_task_energy()` assigns category costs; `accumulate_runtime_energy()` aggregates them.
- [`src/metrics.cpp`](../src/metrics.cpp) owns `modeled_static_power_w()` and `model_accounting()`. The legacy CLI and the optional study observer call this shared accounting implementation.
- [`src/study_contract.cpp`](../src/study_contract.cpp) emits factor-level coverage metadata. That metadata describes implemented cost terms; it is not a physical validation result.

## Existing equations and configured terms

Dynamic category energy is accumulated in picojoules (pJ):

```text
E_vector = modeled_vector_operations × vector_op_pj
E_NPU    = modeled_MACs × npu_mac_pj × arithmetic_scale × profile_scale × voltage_ratio²
E_DRAM   = transferred_bytes × dram_byte_pj
E_DMA    = transferred_bytes × dma_byte_pj
E_dynamic = E_vector + E_NPU + E_DRAM + E_DMA
```

`profile_scale` is the operation's declared mapping/profile arithmetic scale; it is 1 when not applicable. The `voltage_ratio²` term is an NPU arithmetic-energy sensitivity only. It does not model frequency, yield, error rates, leakage, or voltage-dependent timing.

The configured static-power equation is:

```text
P_static = static_power_w
         + npu_macs × npu_static_power_per_mac_w
         + rvv_lanes × rvv_static_power_per_lane_w
         + (sram_capacity_bytes / 1024) × sram_static_power_per_kib_w
         + dram_interface_static_power_w
```

All five terms are treated as watts. Current accounting charges this aggregate for the entire finite makespan or selected study observation interval, including periods with no active task. It is not separated into active, idle, leakage, clock, or interface-activity components.

```text
E_static_pJ = P_static_W × interval_us × 1,000,000
E_IT        = E_dynamic + E_static
P_IT_avg    = E_IT_pJ / interval_us / 1,000,000
P_facility  = PUE × P_IT_avg
```

The zero-duration aggregate helper currently returns zero average IT power; a study result with a zero-duration interval marks average power unavailable. The tests preserve these distinct API behaviors.

## Term inventory

The default values below are software defaults in `Config`. The power-budget values are separate explicit settings in `configs/power_budget/manifest.conf`. Unless a row says otherwise, the value is an **assumed scenario coefficient**; this repository does not establish silicon calibration for it.

| Term / parameter | Owner and equation | Unit and default | Workload paths where it contributes | Provenance / evidence and comparison limits |
|---|---|---|---|---|
| Vector arithmetic: `vector_op_pj` | `simulation.hpp`: vector operations × coefficient | pJ/op; default 0.40 | Physical AI RVV stages; `rvv_stream`; profiled operators with vector elements; control tasks when configured | Configured assumption; hand-checkable from declared operation count. No silicon calibration established. Lane/frequency changes affect timing, not this energy-per-operation input. |
| NPU arithmetic: `npu_mac_pj` | `simulation.hpp`: MACs × coefficient × `npu_arithmetic_energy_scale` × profile scale × `arithmetic_voltage_ratio²` | pJ/MAC; defaults: `npu_mac_pj` 0.20, `npu_arithmetic_energy_scale` 1.0, `arithmetic_voltage_ratio` 1.0; profile scale defaults to 1.0 | Physical AI GEMM; profiled operators with MACs. Not applicable to a pure RVV stream. | Configured assumption; profile mapping scale is an input. No silicon calibration established. Scale and voltage ratio affect only this category; voltage ratio does not model circuit behavior. |
| DRAM movement: `dram_byte_pj` | `simulation.hpp`: transferred bytes × coefficient | pJ/byte; default 15.0 | Physical pipeline transfer tasks; profiled `ProfileTransfer` tasks; `rvv_stream` only when `rvv_memory_accounting=extra_traffic` adds bytes | Configured assumption; no target-memory characterization established. Traffic energy is represented. Bandwidth changes transfer time but do not scale this coefficient. |
| DMA movement: `dma_byte_pj` | `simulation.hpp`: transferred bytes × coefficient | pJ/byte; default 1.5 | Same transfer-task paths as DRAM movement; RVV extra-traffic bytes are ledger-only and do not create another scheduled DMA task | Configured assumption; no target-DMA characterization established. Byte activity is represented; transfer setup has no separate energy term. Bandwidth changes do not scale this coefficient. |
| Base static: `static_power_w` | `metrics.cpp`: direct additive term | W; default 0.0; power-budget manifest uses 0.05 | Every workload and accounting interval; charged without checking task or resource activity | Configured assumption. Not decomposed into idle/leakage/clock. |
| NPU capacity: `npu_static_power_per_mac_w` | `npu_macs × coefficient` in `metrics.cpp` | W/MAC; default 0.0; reference manifest uses 0.00003 | Every configured run with its `npu_macs` value, even if that workload path does not execute NPU work | Configured assumption. Prices configured NPU capacity under that scenario; charged for the full interval. This is not gated by NPU utilization. |
| RVV capacity: `rvv_static_power_per_lane_w` | `rvv_lanes × coefficient` in `metrics.cpp` | W/lane; default 0.0; reference manifest uses 0.002 | Every configured run with its `rvv_lanes` value, even if that workload path does not execute vector work | Configured assumption. Prices configured RVV capacity under that scenario; charged for the full interval. This is not gated by RVV utilization. |
| Main SRAM capacity: `sram_static_power_per_kib_w` | `sram_capacity_bytes / 1024 × coefficient` in `metrics.cpp` | W/KiB; default 0.0; reference manifest uses 0.0001 | Every configured run with its `sram_capacity_bytes` value, regardless of observed accesses | Configured assumption. Capacity static term is represented, but SRAM read/write energy is unpriced. This term uses main `sram_capacity_bytes`, not separate resident-weight SRAM capacity. |
| DRAM-interface static: `dram_interface_static_power_w` | Direct additive term in `metrics.cpp` | W; default 0.0; reference manifest uses fixed 0.02 | Every configured run; currently unchanged by bandwidth, interface width, or activity | Configured assumption. A fixed term exists, but bandwidth investment is incompletely costed. |
| PUE / deployment | `metrics.cpp`: PUE × IT energy/power; fleet count, hours, and tariff derive facility kWh and cost | `pue` default 1.0; `deployment_xpu_count` 1; `deployment_hours` 0.0; `electricity_cost_per_kwh` 0.0 | Any workload result with the corresponding facility/deployment settings | Explicit scenario assumptions, not measured facility results. Arithmetic is verifiable; results inherit all IT-model limitations and chosen PUE/tariff. |

## Missing or partial costs

| Cost or behavior | Current status | Implication |
|---|---|---|
| SRAM read/write/access energy | Unpriced | Reuse or tiling may reduce modeled DRAM traffic without accounting for SRAM accesses needed to obtain that reuse. |
| Resident-weight SRAM static cost | Unpriced for `weight_sram_capacity_bytes` | Residency can reduce first/steady-frame DRAM and DMA traffic, while the separate storage capacity is not charged by the static equation. |
| Resident-weight SRAM access energy | Unpriced | Repeated reads and writes to the resident store are absent. |
| DRAM-interface power versus bandwidth | Partial/unpriced scaling | Fixed interface static power does not rise with configured bandwidth; a bandwidth increase can improve latency without an incremental interface-power charge. |
| Separate idle, leakage, clock, and switching power | Not represented separately | The configured static aggregate is charged continuously; it cannot support component-specific idle/leakage conclusions. |
| Frequency/voltage power and timing coupling | Not represented | The voltage ratio changes NPU arithmetic energy by its square but does not change frequency, latency, static power, or reliability. |
| Scheduler/dispatch energy | Not represented | Scheduling changes have modeled timing effects but no per-event energy cost. |
| Transient/peak power | Not represented | Uniform task-energy interpolation is an interval-accounting approximation, not an instantaneous power/activity trace. |

Missing terms must not be interpreted as zero physical cost.

## Zero value, absent cost, and measured zero

- **Configured zero:** the scenario supplies a coefficient of zero. The model then computes zero contribution from that term. This says nothing about whether the physical component consumes energy.
- **Unmodeled cost:** no equation currently represents the physical cost. It cannot be assigned a physical zero and must remain disclosed as missing or partial.
- **Measured zero:** a measurement process reports zero under defined instruments, resolution, conditions, and uncertainty. AxisSim's configured coefficients and unit tests are not such measurements.

The candidate coverage contract in `src/study_contract.cpp` uses `complete`, `partial`, `missing`, and `n/a`, with represented/omitted terms and resolved coefficients. Its `complete` value means complete only for the terms enumerated for that factor with positive resolved coefficients; it does not mean complete physical power accounting. A zero coefficient is placed among omitted terms in that contract. Keep numeric budget status separate from coverage and recommendation eligibility.

### Candidate factor to machine-readable coverage map

The following is the mapping implemented by `build_power_coverage()` for accepted study factors and other explicitly recognized candidate factors. `represented` terms appear only when the resolved coefficient is positive. Therefore the final `power_coverage` value depends on the workload applicability and actual resolved coefficients; the table states the terms and omissions that drive that value, not a fixed classification for every configuration.

| Candidate factor ID | Applicable execution path | Represented terms when priced | Omitted term(s) / status rule |
|---|---|---|---|
| `npu_macs` | Physical AI, or profile containing NPU/MAC work | `static.npu_w_per_mac` | If its coefficient is zero the term is omitted; otherwise `complete` within this factor's narrow cost contract. `n/a` without NPU work. |
| `rvv_lanes` | Physical AI, or profile containing vector work | `static.rvv_w_per_lane` | If its coefficient is zero the term is omitted; otherwise `complete` within this factor's narrow cost contract. `n/a` without vector work. |
| `dram_bandwidth_gbps` | Physical AI or any profiled workload | `static.dram_interface_w`, `dynamic.dram_byte_pj_per_byte`, `dynamic.dma_byte_pj_per_byte` | Always omits `missing.static.dram_interface_w_per_gbps`; result is `partial` if any listed term is represented, otherwise `missing`. |
| `sram_capacity_bytes` | Physical AI only | `static.sram_w_per_kib` | Always omits `missing.dynamic.sram_access_j_per_byte`; `partial` if capacity power is represented, otherwise `missing`. |
| `weight_sram_capacity_bytes`, `npu_weights_resident` | Physical AI or profile with persistent resident weights | `dynamic.dram_byte_pj_per_byte`, `dynamic.dma_byte_pj_per_byte` | Always omits `missing.static.resident_weight_sram_w_per_byte` and `missing.dynamic.resident_weight_sram_access_j_per_byte`; `partial` if any represented term exists, otherwise `missing`. |
| `npu_tile_m`, `npu_tile_n`, `npu_tile_k` | Physical AI only | `dynamic.dram_byte_pj_per_byte`, `dynamic.dma_byte_pj_per_byte` | Always omits `missing.dynamic.sram_access_j_per_byte`; `partial` if any represented term exists, otherwise `missing`. |
| `scheduler_policy` | All supported workload paths | `static.base_power_w`, `dynamic.vector_op_pj_per_op`, `dynamic.npu_mac_pj_per_mac`, `dynamic.dram_byte_pj_per_byte`, `dynamic.dma_byte_pj_per_byte` when applicable | Always omits `missing.dynamic.scheduler_j_per_event`; `partial` if any represented term exists, otherwise `missing`. |
| `rvv_chunk_elements` | Physical AI only | `static.base_power_w`, `dynamic.vector_op_pj_per_op`, `dynamic.npu_mac_pj_per_mac`, `dynamic.dram_byte_pj_per_byte`, `dynamic.dma_byte_pj_per_byte` when applicable | Always omits `missing.dynamic.scheduler_j_per_event`; `partial` if any represented term exists, otherwise `missing`. Other workload paths are `n/a`. |
| `rvv_frequency_ghz`, `npu_frequency_ghz` | A workload path with the matching vector or NPU engine | `static.base_power_w`, `static.rvv_w_per_lane`, `static.npu_w_per_mac` when applicable | Always omits `missing.dynamic.frequency_voltage_coupling` and `missing.static.frequency_voltage_leakage_coupling`; `partial` if any represented term exists, otherwise `missing`. |
| `npu_arithmetic_energy_scale` | Physical AI or profile with NPU/MAC work | `dynamic.npu_mac_pj_per_mac` | If positive, `complete` only for the listed arithmetic-energy factor. `n/a` without NPU work. |
| `arithmetic_voltage_ratio` | Physical AI or profile with NPU/MAC work | `dynamic.npu_mac_pj_per_mac` | Always omits `missing.dynamic.frequency_voltage_coupling` and `missing.static.frequency_voltage_leakage_coupling`; `partial` if arithmetic energy is represented, otherwise `missing`. |
| `control_period_us`, `control_deadline_us` | Applicable only when a control period is configured | None: these are timing/scheduling factors, not hardware-cost factors | Machine coverage is `n/a`. |
| `release_schedule`, `frame_period_us`, `deadline_us` | Workload release/deadline protocol | None: these are workload timing factors, not hardware-cost factors | Machine coverage is `n/a`. |

Applicability follows `build_power_coverage()`'s workload/profile checks, not whether one particular run happened to record a nonzero contribution. In particular, the static accounting equation prices configured NPU, RVV, and main-SRAM capacity on every run even when the selected workload does not execute that engine or access that SRAM. That is an accounting rule, not evidence that the configured capacity is powered in real hardware.

## The 0.23 W reference

`configs/power_budget/manifest.conf` keeps an illustrative **0.23 W average modeled IT-power budget** with these explicit terms:

| Input | Value |
|---|---:|
| Base static power | 0.05 W |
| NPU static power | 0.00003 W/MAC |
| RVV static power | 0.002 W/lane |
| Main SRAM static power | 0.0001 W/KiB |
| DRAM-interface static power | 0.02 W, fixed across bandwidth settings |
| Average IT-power budget | 0.23 W |

A candidate passing this formula is only **provisionally feasible under current modeled power terms**. It is not physically feasible silicon by virtue of this status. The reference coefficients are scenario inputs, not universal or characterized technology values. Legacy MNIST profiles keep their zero-default static terms and disabled budget; the study-only overlay remains a separate input.

## Measurement scope

- Legacy `energy.per_frame_uj` is the simulator's whole-run **dynamic** energy divided by completed vision frames. `energy.it_per_frame_uj` adds configured static energy using the same frame denominator. The GUI's IT-energy KPI displays the latter.
- Finite study measurements use total modeled IT energy over makespan divided by completed work units after drain.
- Sustained study measurements use IT energy integrated over `[t0,t1)` divided by units completed in that same window. The release-cohort latency/deadline measurement remains a separate cohort.
- Study task dynamic energy is assigned at dispatch and spread uniformly over the modeled task interval to integrate window overlap. This conserves modeled energy over adjacent windows when tested, but does not recover a physical switching trace.

Do not compare these metrics unless the numerator scope, denominator, workload, model, power inputs, and interval match. See [model assumptions](model-assumptions.md) and [scheduler ordering](scheduler-ordering.md) for execution and event semantics.
