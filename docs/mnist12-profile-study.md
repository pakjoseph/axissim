# MNIST-12 public-model study

This study uses the checked-in static ONNX MNIST-12 workload profile and the
`profiled_graph` scheduler path. It is an operation/traffic characterization
under AxisSim's explicit architecture assumptions—not a measured runtime,
compiler mapping, or hardware-power result.

Reproduce the 128 points:

```sh
./experiments/run_mnist12_profile_study.sh
./experiments/analyze_mnist12_profile_study.py experiments/mnist12_profile_study.csv
./build/axissim analyze configs/workloads/mnist12_profiled.conf \
  --ranges configs/workloads/mnist12_sensitivity_ranges.csv \
  --csv experiments/mnist12_sensitivity.csv --json experiments/mnist12_sensitivity.json
```

| Configuration | Mean latency | Energy/frame | Reading |
|---|---:|---:|---|
| 8 RVV lanes, 1,024 MACs, 20 GB/s | 56.42 µs | 4.95 µJ | Baseline profile mapping. |
| 16 RVV lanes, 2,048 MACs, 20 GB/s | 54.77 µs | 4.95 µJ | Only 2.9% faster: more arithmetic has little realized value here. |
| 8 lanes, 1,024 MACs, 5 GB/s | 99.95 µs | 4.95 µJ | Data movement materially controls latency. |
| 8 lanes, 1,024 MACs, 20 GB/s, 3× lower NPU arithmetic energy | 56.42 µs | 4.85 µJ | Total energy falls only 2.1% because DRAM dominates. |

The recommendation is bounded but useful: for this profile and mapping, first
improve data movement or reuse; additional MACs and lower arithmetic energy do
not translate proportionally into end-to-end gains. The declared-bounds study
keeps every point deadline-feasible while latency spans 49.17–99.95 µs as DRAM
bandwidth changes from 40 to 5 GB/s.
