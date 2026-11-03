# Ara hardware-cycle reproduction

This procedure produces raw `[hw-cycles]` records from Ara's own Verilator flow. AxisSim also includes a public 4-lane size-control evidence set; it is a narrow compute-envelope check, not a general timing or power validation.

## Pinned target

- Repository: <https://github.com/pulp-platform/ara>
- Revision: `34bd3bc152421b4601a7bf3d6e8e91ffb545c99e`
- Kernel: FP64 `fmatmul`, up to 128 × 128 × 128
- Lane sweep: 2, 4, 8, 16

Ara's benchmark script uses `[hw-cycles]` markers from the RTL simulation, and its repository documents the Verilator build flow. The project exposes 2–16 lane configurations and calculates VLEN from 1024 bits per lane. [Ara documentation](https://github.com/pulp-platform/ara/blob/main/docs/source/modules/ara.md)

## Host prerequisites

Use a Linux or macOS host with **at least 20 GiB free disk space**; a Verilator-capable compiler toolchain; Python 3; GNU make; Git; and Ara's pinned LLVM RISC-V toolchain. The Ara repository's `make all` flow initializes its pinned submodules and builds the required LLVM/toolchain components. Do not substitute an unpinned compiler and label the result as a controlled reproduction.

```sh
git clone --recursive https://github.com/pulp-platform/ara.git /path/to/ara
git -C /path/to/ara checkout 34bd3bc152421b4601a7bf3d6e8e91ffb545c99e
git -C /path/to/ara submodule update --init --recursive
make -C /path/to/ara all
```

The full build is large and may take substantial time. It is intentionally kept outside this repository. In environments where the upstream all-in-one target is unavailable, complete Ara's documented prerequisite/toolchain steps and record the exact build path in the metadata.

For a Linux-compatible, dependency-pinned host environment, build the supplied base image from the AxisSim root:

```sh
docker build --platform linux/amd64 \
  -t axissim-ara-repro:ubuntu22-amd64 \
  -f docker/ara-reproduction.Dockerfile .
```

The Dockerfile supplies the host build dependencies (including Clang, Flex headers, `help2man`, and NumPy); Ara itself remains a separately cloned public source tree pinned to the revision above. On Apple Silicon, `linux/amd64` runs under emulation and can take several hours for the one-time toolchain and Verilator builds.

## Collect raw cycles

The harness does not change Ara sources. It generates a selected 1³–128³ input, compiles the benchmark for a selected lane configuration, runs the Verilated RTL model, preserves stdout, and extracts the `[hw-cycles]` marker. The upper bound fits the pinned testbench's default 1 MiB memory window.

```sh
for lanes in 2 4 8 16; do
  bash tools/reproduce_ara_fmatmul.sh \
    --ara-dir /path/to/ara --lanes "$lanes" \
    --output-dir reference_data/ara/reproductions/raw --build
done
```

For a fixed-configuration size control, use `--matrix-size 32`, `64`, `96`, and `128`. The checked-in 4-lane evidence is in [`reference_data/ara/reproductions/size_controls`](../reference_data/ara/reproductions/size_controls/README.md). When running Docker under emulation, set `AXISSIM_EXECUTION_CONTEXT` to state that explicitly; metadata records it along with the container architecture.

After the first configuration, omit `--build` only if the selected configuration's Verilated binary has already been produced. Preserve every `.raw.log` and `.metadata.txt` file. Treat cycles as primary. Convert them to microseconds only with a clearly stated normalization frequency and record its provenance. AxisSim's 1.35 GHz conversion is explicitly a comparison normalization, not a measured clock rate.

`--build` detects a Verilated binary built for a different lane count and rebuilds that exact, scoped build directory. This avoids accidentally pairing an application compiled for one lane configuration with RTL compiled for another.

## Interpretation boundary

The raw cycles validate a specific Ara revision, benchmark binary, RTL configuration, and simulation path. They do not validate a real physical chip's clock frequency, silicon power, or Velaura hardware. AxisSim should compare lane-scaling shape and stated timing assumptions; it must not be tuned blindly to minimize a single-point error.
