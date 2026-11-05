# Initial architecture study

Run the complete study after building AxisSim:

```sh
./experiments/run_initial_study.sh
```

The script generates three CSV files that can be plotted or reviewed directly.

| Study | Question | Modeled decision supported |
|---|---|---|
| DRAM bandwidth | At what point does the pipeline meet a frame deadline? | Invest in bandwidth, DMA behavior, or reuse before adding compute. |
| NPU MAC count | Does additional matrix throughput improve end-to-end latency? | Identify diminishing returns from a memory-limited mapping. |
| RVV lane count | Does preprocessing throughput matter more than matrix throughput? | Allocate compute resources across heterogeneous engines. |
| RVV VLEN | Does vector capacity reduce strip-mining overhead at fixed lane count? | Relate ISA-visible vector length to timing cost and tail behavior. |
| NPU arithmetic energy | How much of a compute-block energy gain reaches the full pipeline? | Separate Titan Core-inspired arithmetic savings from system-level savings. |

All results are model outputs derived from the configuration assumptions—not measurements of a real SoC. Record the exact configuration and simulator revision with any chart or architectural recommendation.

## Public-model mapping-policy study

```sh
bash experiments/run_mnist12_mapping_policy_study.sh ./build/axissim \
  experiments/mnist12_mapping_policy_study.csv
```

This four-point study holds the static public MNIST-12 operation profile fixed
and compares only named mapping policies: baseline, one legal single-consumer
fusion, two capacity-checked persistent NPU weight tensors, and both together.
Its CSV records traffic, latency, energy, first-frame/steady-state transfer
totals, and the declared bytes avoided by each policy. It is not a compiler
benchmark or a silicon measurement.

## Plot a sweep

The plotting helper requires Python with `matplotlib` available:

```sh
python3 experiments/plot_sweep.py experiments/rvv_lanes_sweep.csv \
  --output experiments/figures/rvv_lanes.png
```

It produces a compact mean/P99-latency and energy-per-frame chart suitable for the README or a technical walkthrough.
# Constrained design-space study

Run the 96-point grid over RVV lanes, DRAM bandwidth, SRAM capacity, and NPU arithmetic energy:

```sh
./experiments/run_design_space.sh
```

The resulting `design_space.csv` contains modeled latency, deadline misses, and energy per frame. For the checked-in baseline mapping, the 3x lower NPU arithmetic-energy scenario changes energy from 107.76 to 103.28 microjoules per frame without changing latency; DRAM bandwidth and RVV lanes determine latency. This is a model finding under the stated assumptions, not a silicon claim.

