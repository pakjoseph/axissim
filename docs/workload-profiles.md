# Workload profiles

AxisSim workload profiles are offline, reviewable descriptions of a public,
static-shape model. They characterize operation count and tensor traffic; they
are not a measured runtime, compiler mapping, or silicon-power result.

The C++ simulator intentionally does not depend on ONNX Runtime. The optional
extractor reads a pinned model and emits `axissim.workload-profile.v1` JSON:

```sh
python3 -m pip install -r tools/requirements-onnx-extractor.txt
python3 tools/extract_onnx_profile.py model.onnx \
  --output reference_data/workloads/example_profile.json \
  --model-name "Example model" \
  --source-url "https://example.invalid/model" \
  --license "upstream license" \
  --static-input input=1x3x224x224
```

Every checked-in profile must retain its source URL, license, SHA-256, opset,
extractor version, input shapes, mapping-rule version, and source-model notes.
Weights are not checked in unless the upstream license explicitly permits it.

Version 1 maps only static-shape `Conv`, `Gemm`, `MatMul`, pooling,
normalization, flatten, and a small set of elementwise operators. Unsupported, custom, dynamic-shape, or ambiguous
operators stop extraction with an error. AxisSim must not assign unsupported
work a zero-cost or generic-NPU mapping.
