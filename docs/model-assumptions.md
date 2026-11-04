# Model assumptions

AxisSim uses one deterministic, non-preemptive discrete-event scheduler for every workload. A transfer uses the DMA+shared-DRAM resource; compute stages use the RVV or NPU resource after their input dependency completes. `fcfs` dispatches by ready time, `fixed_priority` gives `safety_control` precedence at dispatch points, and `edf` dispatches the ready task with the earliest absolute deadline. None can preempt an operation already in flight.

Frames release at `frame_period_us`. Therefore, operations from different frames may overlap when they use distinct resources. `max concurrent tasks` counts simultaneous trace intervals across all resources; utilization is total busy time for a resource divided by simulation makespan.

DRAM bandwidth is modeled as a single serial shared resource. Transfer duration is `dma_setup_us + bytes / bandwidth`; with bandwidth expressed in GB/s, `bytes / GB/s / 1000` produces microseconds.

The NPU model executes one GEMM with `M * N * K` MACs using output-stationary tiles. For each output tile, it transfers the needed A and B subtiles for every K tile, then writes the accumulated output tile. Edge tiles use their actual dimensions. The selected tile's A, B, and accumulator working set must fit in SRAM.

RVV-style stages have an explicitly modeled element count and operations per element. `VLMAX = (VLEN / SEW) * LMUL`; the model adds configurable strip-mined-loop overhead for each `ceil(elements / VLMAX)` iteration. Compute work is limited by lanes and issue rate. The final iteration's active-element ratio is reported as tail utilization.

`npu_weights_resident=1` models a dedicated weight store that holds the complete unique GEMM weight tensor across frames. The first frame loads the unique tensor once; steady-state frames incur no weight traffic. It is distinct from the tiled operand SRAM and requires `weight_sram_capacity_bytes` to hold the full tensor. This is an idealized residency model: it does not yet model eviction, banking, coherency, or multiple model layers.

`dma_double_buffering=1` releases camera frames according to their configured period and lets distinct resources operate concurrently. A value of `0` permits only one frame in the pipeline at a time, providing a serial reference configuration.

`rvv_chunk_elements=0` models each vector pipeline stage as one non-preemptive runtime operation. A positive value partitions preprocessing and postprocessing into chunks that re-enter the scheduler between operations. This bounds control-task blocking but adds strip-mining/runtime overhead and can increase vision latency.

Energy is accumulated as configured activity counts times pJ/action. `npu_arithmetic_energy_scale` affects only NPU arithmetic energy. It must not be interpreted as a whole-system power reduction.

`rvv_memory_accounting=pipeline_dma` is the default for the Physical AI pipeline: its DMA stages already own modeled DRAM traffic. `extra_traffic` adds `rvv_memory_bytes_per_element` once for every RVV preprocess, postprocess, or standalone stream element. This is an energy/traffic ledger choice; it does not add a second scheduled DMA transfer or DRAM-contention delay.

`arithmetic_voltage_ratio` is an optional voltage-inspired NPU arithmetic-energy sensitivity: AxisSim multiplies NPU arithmetic energy by the square of this ratio. It does not alter frequency, latency, yield, soft-error behavior, timing closure, or reliability, and is not a low-voltage circuit model.

## Hand-verifiable validation case

`tests/tiny_pipeline.conf` is a one-frame regression configuration with a manually derived end-to-end latency of **0.281 µs**: 0.100 µs camera ingest, 0.100 µs preprocessing read, 0.006 µs RVV preprocessing, 0.050 µs write, 0.012 µs NPU-tile operands, 0.008 µs GEMM, 0.001 µs postprocess read, and 0.004 µs RVV postprocessing. CTest verifies the two-decimal rendered result, 0.28 µs.
