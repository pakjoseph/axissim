# Weight-residency break-even study

## Research question

For the checked-in 30-frame MNIST-12 profile and one fixed AxisSim architecture, how much additional unpriced energy could persistent NPU-weight storage consume before its modeled DRAM/DMA savings no longer offset that cost?

This study reports a break-even threshold. It does not estimate the physical SRAM cost or establish that residency is energy-efficient in silicon.

## Preregistered comparison

Both arms use `configs/workloads/mnist12_profiled.conf`, its checked-in MNIST-12 workload profile, the same architecture and implementation settings, 30 periodic frames, and an explicitly matched `weight_sram_capacity_bytes=16384` setting. The treatment is the mapping plan:

- **No persistent residency:** `configs/mappings/mnist12_baseline.json`.
- **Persistent residency:** `configs/mappings/mnist12_persistent_weights.json`, which marks `Convolution28` and `Convolution110` resident.
- **Capacity negative control:** the persistent plan with an 8192-byte store. C++ must reject it before scheduling because the two named weights require 13,600 bytes.

The common 16 KiB capacity makes the comparison an evaluation-policy change on one configured architecture. It does not compare two physically characterized SRAM designs. The no-residency arm leaves the declared store unused.

The first frame loads the same weights in both arms. The C++ ledger must report 13,600 fewer steady-frame bytes in the persistent arm, and the total finite-run difference must equal `13,600 × (30 - 1) = 394,400` logical bytes. This is an aggregate ledger and run-total check; the CLI does not expose frame-indexed byte counts. The CLI reports this payload as modeled DRAM traffic. Its dynamic energy accounting also applies the DMA byte coefficient to the same transfer-byte count; it does not expose a separate physical DMA-byte counter.

## Energy boundary and break-even calculation

For each matched pair, use the C++ result components and finite-run completed-frame count:

```text
additional_SRAM_energy_budget_pJ
  = modeled_IT_energy_no_residency_pJ
  - modeled_IT_energy_persistent_pJ

average_budget_pJ_per_completed_frame
  = additional_SRAM_energy_budget_pJ / 30
```

The budget is the maximum *incremental* unpriced cost attributable to using persistent storage, over this finite 30-frame observation, that would erase the paired modeled IT-energy difference. It may include any incremental resident-store dynamic and static cost; the model currently prices neither resident-weight SRAM accesses nor its dedicated capacity. No per-access break-even result will be reported because the simulator does not expose validated resident-SRAM access counts.

The primary accounting uses the existing configured MNIST-12 energy inputs. The study explicitly sets all five supported static-power inputs to zero, matching the current resolved defaults. That is a zero modeled contribution, not evidence of zero physical static power. The report will separately show dynamic energy, static energy, and IT energy. It will not label absent resident-weight SRAM cost as zero.

Declared DRAM and DMA energy coefficients will also receive a one-at-a-time factor-of-two sensitivity: each coefficient is evaluated at 0.5×, 1×, and 2× its checked-in default while the other remains at default. These are arithmetic stress points selected for sensitivity analysis, not measured values or a physical uncertainty interval. Timing inputs and workload remain fixed. No SRAM coefficient is introduced.

## Hypotheses

1. First-frame logical transfer bytes are equal in both arms.
2. The first-frame ledgers match; the steady-state ledger differs by 13,600 bytes; the run-total difference is 394,400 bytes, consistent with 29 post-first frames. These are aggregate fields, not frame-indexed byte observations.
3. The modeled DRAM and DMA dynamic-energy reductions equal the saved logical payload multiplied by their respective declared coefficients. Vector and NPU arithmetic energy remain equal.
4. Under the same timing configuration, residency does not increase C++-reported mean latency, P99 latency, or deadline misses. The observed result remains a model output, not a hardware latency claim.
5. The total incremental-SRAM-energy budget is positive for positive configured transfer-energy coefficients. Its response to the declared coefficient sensitivities will be checked against independent hand arithmetic. Whether the real missing SRAM cost falls below that threshold is not determined by this study.

A failed hypothesis will be reported as a failed result and investigated; the runner will not rewrite the comparison or expected values to force agreement.

## Measurement and reporting scope

This is a finite run of 30 scheduled and completed frames, not a sustained `[t0,t1)` observer measurement. Energy-per-frame fields use the C++ result's completed-frame denominator. Mean/P99 latency and deadline outcomes come directly from the C++ JSON. The trace is retained for timing and deterministic-replay evidence; byte totals come from the C++ transfer ledger and total traffic field, not from inferring bytes from trace durations.

The machine-readable result, summary CSV, Markdown report, raw C++ JSON, provenance sidecars, and task traces are published together. Provenance is required to identify the executable and all resolved configuration/workload/mapping inputs. The base pair is run once and then each arm receives one additional same-binary replay; result JSON, provenance sidecars, and trace bytes must match exactly.

## Limits

- The workload is a checked-in static operator/traffic profile, not a measured inference execution or compiler trace.
- Transfer bytes are logical modeled payload. They are not DRAM commands or independently observed DMA bus traffic.
- The model does not price resident-weight SRAM read/write energy or dedicated weight-store static/leakage energy.
- The break-even amount is an upper budget for all incremental unpriced SRAM energy at the declared assumptions, not an estimate of SRAM cost.
- Configured zero static coefficients are not measured zeros.
- The factor-of-two coefficient points are user-selected sensitivity tests, not sourced hardware ranges.
- No claim about physical power, energy, SRAM capacity cost, or silicon superiority is supported.

## Reproduction

From the implementation revision, use a clean detached worktree, build the CLI from that exact revision, and write results outside the source checkout. For example:

```sh
git worktree add --detach /tmp/axissim-m8-source <study-source-revision>
cmake -S /tmp/axissim-m8-source -B /tmp/axissim-m8-build -DAXISSIM_BUILD_GUI=OFF
cmake --build /tmp/axissim-m8-build --parallel
ctest --test-dir /tmp/axissim-m8-build --output-on-failure
cd /tmp/axissim-m8-source
python3 experiments/weight_residency_break_even/run_study.py \
  --binary /tmp/axissim-m8-build/axissim \
  --output-dir /tmp/axissim-weight-residency-study
```

Run the Python command from the worktree root (or use the absolute path to `run_study.py`). The runner requires a clean source tree and complete provenance that matches the current Git revision and invoked executable hash. It refuses to overwrite an existing output directory or write inside the checkout. It writes the study manifest, all successful CLI JSON/provenance/trace artifacts, the capacity rejection record, a machine-readable JSON and CSV summary, a concise report, and a SHA-256 inventory. Decimal summary values are serialized as decimal strings to avoid binary floating-point reformatting. The inventory excludes itself.

Focused contracts run through CTest as `weight_residency_break_even_contract`; the Python tests use independent hand arithmetic, invoke the actual C++ CLI across the sensitivity cases and capacity control, and exercise the complete publishing runner from a clean checkout. The clean-checkout runner test skips explicitly when the source tree is modified; use the clean worktree procedure above to execute it.
