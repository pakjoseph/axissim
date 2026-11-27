# M8.0 weight-residency break-even results

Study status: **complete**

These are C++ simulator outputs under the declared MNIST-12 profile assumptions. They are not measured hardware latency, memory behavior, or energy.

## Paired results

| Case | DRAM coefficient (pJ/B) | DMA coefficient (pJ/B) | Logical bytes saved | DRAM energy saved (µJ) | DMA term saved (µJ) | Static energy saved (µJ) | Maximum unpriced SRAM cost over 30 frames (µJ) | Allowance per completed frame (µJ) | Mean latency reduction (µs) | P99 reduction (µs) | Deadline misses avoided |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| base | 15.0 | 1.5 | 394400 | 5.916000 | 0.591600 | 0.000000 | 6.507600 | 0.216920 | 0.657333 | 0.306540 | 0 |
| dram_half | 7.50 | 1.5 | 394400 | 2.958000 | 0.591600 | 0.000000 | 3.549600 | 0.118320 | 0.657333 | 0.306540 | 0 |
| dram_double | 30.0 | 1.5 | 394400 | 11.832000 | 0.591600 | 0.000000 | 12.423600 | 0.414120 | 0.657333 | 0.306540 | 0 |
| dma_half | 15.0 | 0.75 | 394400 | 5.916000 | 0.295800 | 0.000000 | 6.211800 | 0.207060 | 0.657333 | 0.306540 | 0 |
| dma_double | 15.0 | 3.0 | 394400 | 5.916000 | 1.183200 | 0.000000 | 7.099200 | 0.236640 | 0.657333 | 0.306540 | 0 |

## Primary pair details

- The finite run completed 30 frames in both arms.
- First-frame ledger: 290176 B in each arm.
- C++ steady-state ledger: 290176 B without residency and 276576 B with residency. This is an aggregate accounting field, not a frame-indexed trace.
- Total logical DRAM payload: 8705280 B without residency and 8310880 B with residency.
- Modeled DMA energy uses the same transfer-byte counter; the 394400 B DMA payload reduction is not an independent physical bus measurement.
- Makespan: 966713.506800 µs without residency and 966712.826800 µs with residency.
- Mean/P99 frame latency: 56.419533/56.506800 µs without residency and 55.762200/56.200260 µs with residency.
- Deadline misses: 0/30 without residency and 0/30 with residency.
- Static energy difference reported by the model: 0.000000 µJ. Static coefficients in this MNIST profile are configured as zero; this is not a measured physical zero.
- At the declared 15.0 pJ/B DRAM and 1.5 pJ/B DMA coefficients, the finite-run incremental unpriced SRAM-energy budget is 6.507600 µJ total, or 0.216920 µJ per completed frame on average.

## Preregistered hypothesis outcomes

- **H1: supported** — first-frame payload equality across all coefficient settings.
- **H2: supported** — first-frame, steady-state ledger, and total-run traffic aggregates reconcile with the declared weight reuse.
- **H3: supported** — reported dynamic energy reduction reconciles with the DRAM and DMA categories; independent tests check each coefficient against the byte-savings arithmetic.
- **H4: supported** — C++ latency and deadline outcomes do not worsen under the matched mapping change.
- **H5: supported** — the modeled finite-interval IT difference leaves a positive upper budget for unpriced incremental SRAM energy across the declared sensitivity cases; actual SRAM cost remains unknown.

## Capacity control

The 8192 B persistent-store control was **rejected_as_expected**; it requires 13600 B and returned process status 1. No result JSON or trace was emitted for this invalid configuration.
The accepted configuration declares 16384 B for 13600 B of persistent weights (2784 B of declared capacity remaining). These are model configuration values, not physical macro area or cost.

## Interpretation and limits

The break-even amount is a threshold for the sum of all incremental, currently unpriced resident-weight SRAM energy over the matched observation. No SRAM access count, per-access energy, resident-store static-power term, or capacity-energy coefficient is available, so the study cannot say whether real SRAM cost is below the threshold. The 16 KiB capacity is an explicit modeled configuration, not a physically characterized macro.

The transfer-byte ledger is logical modeled payload. Existing C++ energy accounting applies both DRAM and DMA coefficients to that same payload count. Sensitivity points vary declared energy coefficients only; they do not represent measured technology uncertainty. No physical DRAM command, power, energy, silicon latency, or hardware-superiority claim follows.
