#!/usr/bin/env python3
"""Run a supplied static-shape ONNX model on the local CPU and retain evidence.

This optional harness runs ONNX Runtime's CPUExecutionProvider on the host that
invokes it. It is deliberately cross-platform (x86-64 and Arm64) and records
host/runtime/model provenance with latency statistics. It does not estimate
chip power, infer a target architecture, or calibrate AxisSim automatically.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import sys
import time
from typing import Any


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def concrete_shape(shape: list[Any], dynamic_dimension: int) -> list[int]:
    resolved: list[int] = []
    for dimension in shape:
        if isinstance(dimension, int) and dimension > 0:
            resolved.append(dimension)
        else:
            resolved.append(dynamic_dimension)
    return resolved


def numpy_type(onnx_type: str, np):
    types = {
        "tensor(float)": np.float32,
        "tensor(double)": np.float64,
        "tensor(float16)": np.float16,
        "tensor(int64)": np.int64,
        "tensor(int32)": np.int32,
        "tensor(int8)": np.int8,
        "tensor(uint8)": np.uint8,
        "tensor(bool)": np.bool_,
    }
    if onnx_type not in types:
        raise ValueError(f"unsupported ONNX input type {onnx_type!r}; supply a model with numeric tensor inputs")
    return types[onnx_type]


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    position = (len(ordered) - 1) * fraction
    lower, upper = int(position), int(position + 0.999999999)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=pathlib.Path, help="static or dynamically-shaped ONNX model to benchmark")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="evidence JSON output path")
    parser.add_argument("--model-name", required=True)
    parser.add_argument("--model-source-url", required=True)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--dynamic-dimension", type=int, default=1,
                        help="replacement for every unresolved input dimension (default: 1)")
    parser.add_argument("--intra-op-threads", type=int, default=0,
                        help="ONNX Runtime CPU thread count; 0 leaves the runtime default")
    parser.add_argument("--energy-joules", type=float,
                        help="optional externally measured whole-system energy for all timed iterations")
    parser.add_argument("--energy-method", default="",
                        help="required provenance label when --energy-joules is supplied")
    parser.add_argument("--notes", default="No OS affinity, scheduling priority, thermal control, or power lock was applied.",
                        help="plain-language run conditions retained with the evidence")
    arguments = parser.parse_args()
    if not arguments.model.is_file():
        parser.error(f"model does not exist: {arguments.model}")
    if arguments.warmup < 0 or arguments.iterations < 2 or arguments.dynamic_dimension < 1 or arguments.intra_op_threads < 0:
        parser.error("warmup must be non-negative; iterations must be at least two; dimensions and threads must be positive when set")
    if (arguments.energy_joules is None) != (arguments.energy_method == ""):
        parser.error("--energy-joules and --energy-method must be supplied together")
    if arguments.energy_joules is not None and arguments.energy_joules <= 0.0:
        parser.error("--energy-joules must be positive")
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print("error: install optional benchmark requirements: python3 -m pip install -r tools/requirements-onnx-benchmark.txt", file=sys.stderr)
        return 2

    try:
        session_options = ort.SessionOptions()
        if arguments.intra_op_threads:
            session_options.intra_op_num_threads = arguments.intra_op_threads
        session = ort.InferenceSession(str(arguments.model), session_options, providers=["CPUExecutionProvider"])
        if session.get_providers() != ["CPUExecutionProvider"]:
            raise ValueError(f"CPUExecutionProvider was not selected; active providers: {session.get_providers()}")
        inputs = {}
        input_evidence = []
        for model_input in session.get_inputs():
            shape = concrete_shape(list(model_input.shape), arguments.dynamic_dimension)
            value = np.zeros(shape, dtype=numpy_type(model_input.type, np))
            inputs[model_input.name] = value
            input_evidence.append({"name": model_input.name, "onnx_type": model_input.type, "shape": shape})
        if not inputs:
            raise ValueError("model has no tensor inputs")
        for _ in range(arguments.warmup):
            session.run(None, inputs)
        samples_us = []
        for _ in range(arguments.iterations):
            start_ns = time.perf_counter_ns()
            session.run(None, inputs)
            samples_us.append((time.perf_counter_ns() - start_ns) / 1000.0)
    except (ValueError, ort.OrtRuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    evidence = {
        "schema_version": "axissim.host-reference.v1",
        "status": "reproduced",
        "measurement_scope": "host CPU inference latency; not target-silicon power, an AxisSim calibration, or a cross-architecture performance ranking",
        "host": {
            "architecture": platform.machine(),
            "processor": platform.processor() or "not reported by Python",
            "platform": platform.platform(),
            "python": platform.python_version(),
            "logical_cpu_count": os.cpu_count(),
        },
        "runtime": {
            "name": "ONNX Runtime",
            "version": ort.__version__,
            "execution_provider": "CPUExecutionProvider",
            "intra_op_threads": arguments.intra_op_threads or "runtime_default",
        },
        "model": {
            "name": arguments.model_name,
            "source_url": arguments.model_source_url,
            "sha256": sha256(arguments.model),
            "inputs": input_evidence,
        },
        "measurement": {
            "recorded_at_utc": datetime.datetime.now(datetime.UTC).isoformat(),
            "warmup_iterations": arguments.warmup,
            "timed_iterations": arguments.iterations,
            "latency_us": {
                "mean": statistics.fmean(samples_us),
                "p50": percentile(samples_us, 0.50),
                "p99": percentile(samples_us, 0.99),
                "min": min(samples_us),
                "max": max(samples_us),
            },
            "raw_samples_us": samples_us,
            "notes": arguments.notes,
        },
    }
    if arguments.energy_joules is not None:
        evidence["external_energy"] = {
            "joules_for_timed_iterations": arguments.energy_joules,
            "method": arguments.energy_method,
            "energy_per_inference_uj": arguments.energy_joules * 1_000_000.0 / arguments.iterations,
            "scope": "Externally supplied measurement; may include components beyond CPU/model execution.",
        }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote host CPU reference evidence to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
