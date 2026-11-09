# Cross-architecture host references

This directory stores provenance-rich latency records created by
`tools/benchmark_onnx_cpu.py` on an actual host CPU. The harness supports
x86-64 and Arm64 systems on macOS, Windows, and Linux through ONNX Runtime's
`CPUExecutionProvider`.

These records are **not** automatically AxisSim calibration data. A general
CPU running an ONNX Runtime graph differs materially from AxisSim's explicitly
modeled RVV/NPU pipeline. Use them to preserve reproducible real-host evidence,
compare software/runtime configurations on the same host, and bound future
architecture mappings. Only add a record to `axissim calibrate` when the
kernel, mapping, frequency normalization, and modeled resources are genuinely
comparable.

## Capture procedure

1. Obtain a legally redistributable ONNX model and record its source URL.
2. Install the optional dependencies:

   ```sh
   python3 -m pip install -r tools/requirements-onnx-benchmark.txt
   ```

   The checked-in reference contract pins ONNX Runtime 1.31.0. Upgrade it only
   when recapturing every record intended for a strict comparison.

3. Run the same command on each target host. Example for a static MobileNet
   model:

   ```sh
   python3 tools/benchmark_onnx_cpu.py model.onnx \
     --model-name "MobileNetV2 batch-1" \
     --model-source-url "https://github.com/onnx/models" \
     --warmup 20 --iterations 100 \
     --output reference_data/hosts/my-host-mobilenet.json
   ```

The JSON captures model SHA-256, model inputs, CPU architecture, OS, Python and
ONNX Runtime versions, provider, thread setting, raw timed samples, timestamp,
run notes, and mean/P50/P99 latency. It never invents a wattage. If an external meter reports whole-system energy, pass
both `--energy-joules` and a precise `--energy-method`; the output labels its
scope accordingly.

## Comparability rules

- Compare records only when the model hash, shapes, provider, warm-up policy,
  timed iterations, and thread policy are understood.
- Inspect raw samples and document desktop load, thermal state, affinity, and
  power policy in `measurement.notes`; a noisy host run is evidence of its
  environment, not a stable hardware characterization.
- Treat host CPU latency as an empirical software-stack result, not a CPU
  microarchitecture ranking or an NPU result.
- Treat externally supplied energy as meter-scoped evidence, not chip power.
- Keep raw, local host evidence out of a pull request unless it is reproducible
  and its source machine/configuration is safe to publish.

## Included records

Both checked-in records run the same public MobileNetV2 opset-7 model (batch 1,
SHA-256 `c1c513…cd042f`) with ONNX Runtime 1.31.0 and the CPU provider, after
20 warm-up and 100 timed iterations.

- `apple-silicon-arm64-mobilenetv2-7.json` is an interactive macOS Arm64 host
  capture. It reports 7,894.58 µs P50 and 18,552.56 µs P99.
- `github-actions-ubuntu-x86_64-mobilenetv2-7.json` is a GitHub-hosted shared
  Ubuntu x86-64 runner capture. It reports 2,134.24 µs P50 and 5,702.36 µs
  P99. The workflow at `.github/workflows/capture-host-reference.yml` can
  reproduce the artifact, but a shared CI VM is not a stable CPU benchmark.

They satisfy this repository's **latency-contract** checks for an illustrative
same-model/runtime comparison. They do not support a cross-vendor CPU ranking,
an energy comparison, or an AxisSim calibration claim.
