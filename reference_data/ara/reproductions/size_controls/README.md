# Ara 4-lane size controls

This evidence set holds a fixed-Ara-configuration FP64 fmatmul size sweep: 32 cubed, 64 cubed, 96 cubed, and 128 cubed at four lanes and VLEN 4096.

It exists to separate total RTL scaling from AxisSim's compute-only envelope. It is not a cache, memory, power, or silicon-validation study. The 128 cubed run is a deterministic rerun of the earlier four-lane point.

| Size | Raw RTL cycles | AxisSim compute cycles |
|---:|---:|---:|
| 32 cubed | 11,221 | 8,192 |
| 64 cubed | 69,857 | 65,536 |
| 96 cubed | 226,255 | 221,184 |
| 128 cubed | 532,392 | 524,288 |

An ordinary least-squares fit of the four raw points against the FMA-operation count is:

~~~
RTL cycles = 3247.934 + 0.126149485 × FMA operations
~~~

AxisSim's frozen compute-only slope is 0.125 cycles per operation, with no fixed system overhead. That closeness is a size-control result for this narrow workload/configuration, not a general Ara timing-accuracy claim.
