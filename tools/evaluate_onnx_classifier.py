#!/usr/bin/env python3
"""Compare two supplied ONNX classifiers and emit AxisSim accuracy evidence.

This intentionally evaluates models supplied by the user; it does not perform
quantization, download a dataset, or infer any accuracy result from AxisSim's
analytical performance model. The dataset is a local NPZ with an input array
and integer labels, making the evaluation data and preprocessing explicit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=pathlib.Path, help="baseline ONNX classifier")
    parser.add_argument("candidate", type=pathlib.Path, help="separately transformed ONNX classifier")
    parser.add_argument("dataset", type=pathlib.Path, help="local .npz with inputs and labels arrays")
    parser.add_argument("--mapping-plan-id", required=True)
    parser.add_argument("--source-model-sha256", required=True, help="hash recorded by the AxisSim workload profile")
    parser.add_argument("--dataset-name", required=True)
    parser.add_argument("--dataset-source-url", required=True)
    parser.add_argument("--input-key", default="inputs")
    parser.add_argument("--labels-key", default="labels")
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if len(args.source_model_sha256) != 64 or args.batch_size < 1:
        parser.error("source-model-sha256 must be 64 characters and batch-size must be positive")
    if not all(path.is_file() for path in (args.baseline, args.candidate, args.dataset)):
        parser.error("baseline, candidate, and dataset must be existing files")
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print("error: install optional evaluator requirements: python3 -m pip install -r tools/requirements-onnx-evaluator.txt", file=sys.stderr)
        return 2

    archive = np.load(args.dataset, allow_pickle=False)
    if args.input_key not in archive or args.labels_key not in archive:
        parser.error("dataset NPZ must contain the declared input and labels keys")
    inputs, labels = archive[args.input_key], archive[args.labels_key]
    if inputs.shape[0] == 0 or labels.ndim != 1 or inputs.shape[0] != labels.shape[0]:
        parser.error("inputs must be non-empty and labels must be one-dimensional with matching sample count")

    def accuracy(model: pathlib.Path) -> float:
        session = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
        model_inputs, model_outputs = session.get_inputs(), session.get_outputs()
        if len(model_inputs) != 1 or len(model_outputs) != 1:
            raise ValueError("evaluator supports exactly one classifier input and one logits output")
        predictions: list[np.ndarray] = []
        for start in range(0, len(inputs), args.batch_size):
            output = session.run([model_outputs[0].name], {model_inputs[0].name: inputs[start:start + args.batch_size]})[0]
            if output.ndim != 2 or output.shape[0] != min(args.batch_size, len(inputs) - start):
                raise ValueError("classifier output must be [batch, classes] logits")
            predictions.append(np.argmax(output, axis=1))
        return float(np.mean(np.concatenate(predictions) == labels))

    try:
        baseline_accuracy = accuracy(args.baseline)
        candidate_accuracy = accuracy(args.candidate)
    except (ValueError, ort.OrtRuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    evidence = {
        "schema_version": "axissim.accuracy-evidence.v1",
        "evaluation_status": "external_evaluated",
        "mapping_plan_id": args.mapping_plan_id,
        "source_model_sha256": args.source_model_sha256,
        "metric": "top1_accuracy_fraction",
        "value": candidate_accuracy,
        "baseline_value": baseline_accuracy,
        "candidate_minus_baseline": candidate_accuracy - baseline_accuracy,
        "dataset": args.dataset_name,
        "source_url": args.dataset_source_url,
        "evaluator": f"tools/evaluate_onnx_classifier.py; onnxruntime {ort.__version__}; CPUExecutionProvider; batch_size={args.batch_size}",
        "baseline_model_sha256": sha256(args.baseline),
        "candidate_model_sha256": sha256(args.candidate),
        "dataset_sha256": sha256(args.dataset),
        "scope": "Externally evaluated supplied models only; no claim about AxisSim timing, energy, compiler lowering, or target-silicon accuracy.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote externally evaluated accuracy evidence to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
