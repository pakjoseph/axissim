# Ara RVV reference data

This directory stores only traceable public or locally reproduced reference points used by AxisSim calibration.

The initial target is [Ara/Ara2](https://github.com/pulp-platform/ara), an open RVV processor with configurations from 2 to 16 lanes. Public publications describe lane/configuration scaling, but machine-readable per-kernel latency data must be captured from a cited figure/table or an explicitly pinned local reproduction before it is added here.

Each row must identify a source URL, revision/date, benchmark, lane count, frequency, workload mapping, unit, and status. Do not enter estimated values or values inferred from a chart without marking their extraction method. `published` means the value is traceable to a public figure, table, or artifact. `reproduced` means an AxisSim maintainer ran a pinned public source revision and retained the command and raw output alongside the data.

`axissim calibrate` validates every schema column. Rows must use `published` or `reproduced` status, use `us` as the latency unit, and contain positive lanes, frequency, and latency. It produces predictions using the row lane count; the other fields establish comparable provenance and workload intent. Keep the reference workload and the AxisSim scenario mapping comparable—this tool evaluates a stated model mapping, not arbitrary application performance.
