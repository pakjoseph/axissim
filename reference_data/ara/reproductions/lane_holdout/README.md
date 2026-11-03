# Ara lane holdout

This directory contains held-out lane-configuration checks for the frozen AxisSim compute-only RVV stream model. The model uses two FP64 operations per lane per cycle and zero strip-mining overhead; those assumptions were not retuned after the 4-lane size-control study.

| Ara lanes | VLEN | Raw RTL cycles | AxisSim cycles | Relative error |
|---:|---:|---:|---:|---:|
| 2 | 2048 | 1,064,052 | 1,048,576 | 1.454% |
| 8 | 8192 | 275,911 | 262,144 | 4.990% |

The result is a public RTL simulation of a pinned Ara revision and FP64 128 cubed fmatmul kernel. It is evidence about this narrow compute envelope, not silicon timing, system-memory behavior, or power. Additional lane configurations belong in this table only after a configuration-specific Verilated binary and matching metadata are preserved.
