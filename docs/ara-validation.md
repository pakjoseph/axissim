# Ara RTL validation report

## Question

Can AxisSim's frozen RVV compute-envelope model predict the scaling trend of a public vector-processor RTL benchmark without using proprietary data?

## Evidence and protocol

The target is the public Ara repository at revision `34bd3bc152421b4601a7bf3d6e8e91ffb545c99e`, using its FP64 128 cubed `fmatmul` benchmark in Verilator. Raw `[hw-cycles]` are the primary metric. The 1.35 GHz value in the reference CSV is only a common normalization for displaying microseconds; it is not a measured clock.

AxisSim uses a frozen compute envelope of two FP64 operations per lane per cycle, with no fitted strip-mining overhead. Four 4-lane matrix sizes were used only to check the arithmetic scaling slope. The 2-, 8-, and 16-lane 128 cubed configurations were then evaluated as held-out lane points.

## Results

| Lanes | RTL cycles | AxisSim cycles | Difference |
|---:|---:|---:|---:|
| 2 | 1,064,052 | 1,048,576 | 1.454% |
| 4 | 532,392 | 524,288 | 1.522% |
| 8 | 275,911 | 262,144 | 4.990% |
| 16 | 172,252 | 131,072 | 23.907% |

The 4-lane size controls fit `RTL cycles = 3247.934 + 0.126149485 × operation count`; the frozen AxisSim slope is `0.125` cycles per operation. This is strong evidence for the narrow arithmetic-rate envelope, not for system-level timing.

At 16 lanes, the idealized model predicts a larger speedup than RTL delivers. That is the useful architectural finding: as peak compute rises, fixed and lane-scaling effects omitted from a pure throughput model become a larger fraction of total execution time.

## Interpretation boundary

This work does not validate Ara silicon timing, power, memory hierarchy, or Velaura hardware. It does not establish a universal Ara timing model. The checked-in raw logs and metadata make every result traceable to its source revision, lane configuration, compiler, Verilator build, hashes, and execution context.

## Next model step

Keep `rvv_stream` as the uncalibrated compute envelope. Add a separate optional affine-overhead model only after collecting more than one matrix size at the held-out lane configurations. That separation prevents a fitted model from being presented as an independent prediction.
