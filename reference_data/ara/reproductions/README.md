# Reproduced Ara evidence

Store only generated raw `[hw-cycles]` logs and matching metadata from `tools/reproduce_ara_fmatmul.sh` here.

Included evidence: `ara_fmatmul64_128_4lane.raw.log` is a successful FP64 128³ run of Ara revision `34bd3bc152421b4601a7bf3d6e8e91ffb545c99e`, using its 4-lane / 4096-bit-VLEN configuration. It reports `532392` hardware cycles and carries hashes, toolchain context, and normalization boundary in its matching metadata file.

For every imported calibration row, retain:

- the unedited raw simulator log;
- the metadata file with repository revision, lane count, matrix dimensions, and extracted cycle count;
- the selected frequency and the arithmetic used to convert cycles to microseconds.

See [the reproduction guide](../../../docs/ara-reproduction.md).
