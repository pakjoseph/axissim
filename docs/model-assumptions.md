# Model assumptions

AxisSim reserves resources on a deterministic simulated-time calendar. A transfer reserves both the DMA engine and the shared DRAM interface. Compute stages reserve their relevant engine after their input dependency completes.

Frames release at `frame_period_us`. Therefore, operations from different frames may overlap when they use distinct resources. `max concurrent tasks` counts simultaneous trace intervals across all resources; utilization is total busy time for a resource divided by simulation makespan.

DRAM bandwidth is modeled as a single serial shared resource. Transfer duration is `dma_setup_us + bytes / bandwidth`; with bandwidth expressed in GB/s, `bytes / GB/s / 1000` produces microseconds.

The NPU model executes one GEMM with `M * N * K` MACs using output-stationary tiles. For each output tile, it transfers the needed A and B subtiles for every K tile, then writes the accumulated output tile. Edge tiles use their actual dimensions. The selected tile's A, B, and accumulator working set must fit in SRAM.

RVV-style stages have an explicitly modeled element count and operations per element. `VLMAX = (VLEN / SEW) * LMUL`; the model adds configurable strip-mined-loop overhead for each `ceil(elements / VLMAX)` iteration. Compute work is limited by lanes and issue rate. The final iteration's active-element ratio is reported as tail utilization.

Energy is accumulated as configured activity counts times pJ/action. `npu_arithmetic_energy_scale` affects only NPU arithmetic energy. It must not be interpreted as a whole-system power reduction.

## Hand-verifiable validation case

`tests/tiny_pipeline.conf` is a one-frame regression configuration with a manually derived end-to-end latency of **0.281 µs**: 0.100 µs camera ingest, 0.100 µs preprocessing read, 0.006 µs RVV preprocessing, 0.050 µs write, 0.012 µs NPU-tile operands, 0.008 µs GEMM, 0.001 µs postprocess read, and 0.004 µs RVV postprocessing. CTest verifies the two-decimal rendered result, 0.28 µs.
