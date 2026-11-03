# Ara RVV reference data

This directory stores only traceable public or locally reproduced reference points used by AxisSim calibration.

The initial target is [Ara/Ara2](https://github.com/pulp-platform/ara), an open RVV processor with configurations from 2 to 16 lanes. Public publications describe lane/configuration scaling, but machine-readable per-kernel latency data must be captured from a cited figure/table or an explicitly pinned local reproduction before it is added here.

Each row must identify a source URL, revision/date, benchmark, lane count, frequency, workload mapping, unit, and status. Do not enter estimated values or values inferred from a chart without marking their extraction method. `published` means the value is traceable to a public figure, table, or artifact. `reproduced` means an AxisSim maintainer ran a pinned public source revision and retained the command and raw output alongside the data.

`axissim calibrate` validates every schema column. Rows must use `published` or `reproduced` status, use `us` as the latency unit, and contain positive lanes, frequency, and latency. `axis_config` is resolved relative to the CSV file and selects the matching AxisSim workload profile for that row; use `.` to select the CLI configuration argument. The tool applies the row lane count to that profile. Keep the reference workload and the AxisSim scenario mapping comparable—this tool evaluates a stated model mapping, not arbitrary application performance.

## Included Ara2 point

`ara2_published_latency.csv` contains one source-derived value from Table IV of the [Ara2 paper](https://arxiv.org/abs/2311.07493): a 4-lane design at 1.35 GHz reports 10.7 GOPS for `fmatmul64` on 256×256 matrices. The stored 3,135.928 µs latency is calculated as `(2 × 256³ floating-point operations) ÷ 10.7 GOPS`. The source reports throughput to three significant digits, so this value is appropriate as a roofline-consistency check, not a high-precision cycle-count comparison.

The matching `rvv_stream` configuration models only a vector FMA stream at the source lane count, clock, and arithmetic issue rate. It intentionally excludes the scalar core, cache, vector-memory system, and physical-AI pipeline. To claim lane-scaling validation, add four or more pinned `reproduced` rows from Ara's own `scripts/benchmark.sh`, including the raw hardware-cycle logs.

## Included reproduced RTL point

`ara_rtl_reproduced_latency.csv` contains an AxisSim-maintainer reproduction from the pinned Ara source revision above: FP64 `fmatmul` at 128³, 4 lanes, and VLEN 4096. The unmodified public RTL simulation completed successfully with `532,392` `[hw-cycles]`; its raw log and hashes live in `reproductions/`. The 394.364444 µs CSV value is `532,392 / 1.35 GHz`, used only to compare a stated cycle model at the same nominal normalization frequency. It is not a measured Ara clock period, power result, or silicon-validation claim.
