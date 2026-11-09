#!/usr/bin/env python3
"""Validate checked-in host evidence is complete and contract-compatible."""

from __future__ import annotations

import json
import math
import pathlib
import sys


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def load(path: pathlib.Path) -> dict:
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("schema_version") != "axissim.host-reference.v1" or record.get("status") != "reproduced":
        raise ValueError(f"{path}: invalid host-reference schema/status")
    measurement = record.get("measurement", {})
    samples = measurement.get("raw_samples_us", [])
    latency = measurement.get("latency_us", {})
    if not isinstance(samples, list) or len(samples) != measurement.get("timed_iterations") or len(samples) < 2:
        raise ValueError(f"{path}: raw samples must reconcile to timed iterations")
    for key, fraction in (("mean", None), ("p50", 0.50), ("p99", 0.99), ("min", None), ("max", None)):
        expected = sum(samples) / len(samples) if key == "mean" else percentile(samples, fraction) if fraction is not None else min(samples) if key == "min" else max(samples)
        if abs(latency.get(key, float("nan")) - expected) > 1e-6:
            raise ValueError(f"{path}: reported {key} does not reconcile to raw samples")
    return record


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_host_reference_records.py ARM_RECORD X86_RECORD")
    arm, x86 = (load(pathlib.Path(argument)) for argument in sys.argv[1:])
    if arm["host"].get("architecture") != "arm64" or x86["host"].get("architecture") != "x86_64":
        raise SystemExit("expected Arm64 and x86-64 evidence records")
    for path, record in zip(sys.argv[1:], (arm, x86)):
        if "not target-silicon power" not in record.get("measurement_scope", ""):
            raise SystemExit(f"{path}: missing measurement-scope boundary")
    contract = (
        ("model", "sha256"), ("model", "inputs"), ("runtime", "name"), ("runtime", "version"),
        ("runtime", "execution_provider"), ("runtime", "intra_op_threads"),
        ("measurement", "warmup_iterations"), ("measurement", "timed_iterations"),
    )
    mismatches = [f"{parent}.{child}" for parent, child in contract if arm[parent].get(child) != x86[parent].get(child)]
    if mismatches:
        raise SystemExit("host reference comparison contract mismatch: " + ", ".join(mismatches))
    print("host reference records reconcile and share a latency comparison contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
