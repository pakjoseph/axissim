# Baseline architecture study

All values below are reproducible outputs from `configs/baseline/manifest.conf`, model version 0.2. They are analytical model results, not measurements or predictions for any real chip.

Regenerate the data and figures with:

```sh
./experiments/run_initial_study.sh
python3 experiments/plot_sweep.py experiments/rvv_lanes_sweep.csv \
  --output experiments/figures/rvv_lanes.png
```

## Memory bandwidth establishes the real-time threshold

| DRAM bandwidth (GB/s) | Mean latency (µs) | Vision deadline misses |
|---:|---:|---:|
| 0.15 | 216,004.84 | 30 / 30 |
| 0.5 | 12,658.15 | 0 / 30 |
| 1 | 6,608.36 | 0 / 30 |
| 5 | 1,768.53 | 0 / 30 |
| 20 | 861.06 | 0 / 30 |

At 0.15 GB/s, the configured frame stream requires more DRAM service than the memory model can sustain. Queues grow across frames and every deadline misses.

## RVV throughput matters more than extra NPU MACs for this workload

Increasing RVV lanes from 2 to 32 reduces mean latency from 2,010.36 µs to 573.74 µs, a 71.5% reduction. By comparison, increasing NPU MACs from 256 to 4,096 reduces latency from 959.36 µs to 836.48 µs, a 12.8% reduction.

The modeled decision is not “never add NPU compute.” It is that this mapping should first address vector preprocessing and memory movement.

## Arithmetic-energy efficiency is not system-energy efficiency

| NPU arithmetic-energy scale | Energy/frame (µJ) | NPU arithmetic energy (µJ) |
|---:|---:|---:|
| 1.0 | 107.76 | 6.71 |
| 0.5 | 104.40 | 3.36 |
| 0.333 | 103.28 | 2.24 |
| 0.25 | 102.73 | 1.68 |

A hypothetical 3× NPU arithmetic-energy reduction lowers modeled total energy by only 3.9%, because DRAM dominates the baseline energy estimate. The result motivates evaluating low-power arithmetic alongside data movement and workload mapping.

## Runtime policy and bounded vector work

Under a deliberately stressed 500 µs vision period and 200 µs control period, fixed priority reduces control deadline misses relative to FCFS and EDF, but long non-preemptive RVV work remains a blocker. Partitioning RVV work into 20,000-element chunks reduces modeled control misses from 45/50 to 0/50, while increasing vision latency.

This illustrates the key real-time tradeoff: scheduling policy helps, but bounded work units or preemption are needed to protect urgent deadlines.
