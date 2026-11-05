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

Profiled scenarios may additionally reference a reviewed
`axissim.mapping-plan.v1` artifact. A mapping plan names every source operator
once and must match the profile's source-model SHA-256. The initial baseline
plan makes existing placement explicit; it does not yet claim arbitrary
cross-engine placement or fusion. Result JSON includes a per-frame transfer
ledger that separates activation reads, activation writes, and weight reads.
That ledger is an accounting basis for later, explicitly declared fusion or
residency policies—not a measured runtime memory trace.

The first supported transformation is a named, direct, single-consumer fusion.
It may remove only the declared intermediate tensor's modeled activation write
and matching consumer read. It does not change operation count, infer a generic
percentage saving, or permit branched/multi-consumer fusion. The checked-in
MNIST `Plus30 → ReLU32` study is an example mapping assumption, not evidence of
a production compiler transformation.

Mapping entries may also declare `"weight_residency": "persistent"` for an
NPU operator with explicitly profiled weights. This is a capacity-checked
mapping assumption: the sum of those named weight tensors must fit the
scenario's `weight_sram_capacity_bytes`. AxisSim keeps the weights in the
first-frame transfer ledger and removes only their declared weight-read bytes
from later frames. Result JSON therefore exposes `first_frame_bytes`,
`steady_state_bytes`, and the named steady-frame bytes avoided. It does not
claim cache behavior, compiler-managed residency, accuracy preservation, or
measured silicon performance.
