#!/usr/bin/env python3
"""Verify deterministic_v1 task traces and CLI results against a portable fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def normalized(data: bytes) -> bytes:
    if os.name == "nt":
        if b"\r" in data.replace(b"\r\n", b""):
            raise RuntimeError("unexpected bare carriage return in output")
        return data.replace(b"\r\n", b"\n")
    return data


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(root: Path, binary: Path, config: str, output: Path) -> tuple[dict, bytes, bytes, bytes]:
    output.mkdir(parents=True, exist_ok=True)
    result_path = output / "result.json"
    trace_path = output / "trace.csv"
    command = [str(binary), "run", config, "--set", "scheduler_ordering_policy=deterministic_v1",
               "--json", str(result_path), "--trace", str(trace_path)]
    completed = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"deterministic run failed ({completed.returncode}): {completed.stderr.decode(errors='replace')}")
    if completed.stderr:
        raise RuntimeError(f"unexpected stderr: {completed.stderr.decode(errors='replace')}")
    result_bytes = normalized(result_path.read_bytes())
    trace_bytes = normalized(trace_path.read_bytes())
    result = json.loads(result_bytes)
    stdout = normalized(completed.stdout.replace(str(trace_path).encode(), b"<TRACE_CSV>")
                        .replace(str(result_path).encode(), b"<RESULT_JSON>"))
    return result, result_bytes, trace_bytes, stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    args = parser.parse_args()
    if sys.version_info < (3, 10):
        parser.error("deterministic scheduler contract requires Python 3.10 or newer")
    root = args.source_root.resolve()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise RuntimeError(f"simulator binary does not exist: {binary}")
    fixture = json.loads(args.fixture.read_text())
    if fixture.get("schema_id") != "axissim.scheduler-ordering.v1" or fixture.get("policy") != "deterministic_v1":
        raise RuntimeError("unsupported deterministic scheduler fixture")

    with tempfile.TemporaryDirectory(prefix="axissim-deterministic-scheduler-") as temporary:
        base = Path(temporary)
        first = run(root, binary, "configs/workloads/mnist12_profiled.conf", base / "first")
        second = run(root, binary, "configs/workloads/mnist12_profiled.conf", base / "second")
        if first != second:
            raise RuntimeError("deterministic MNIST output is not repeatable across independent directories")
        result, result_bytes, trace_bytes, _ = first
        if result.get("schema_version") != "1.3" or result.get("scheduler_ordering_policy") != "deterministic_v1":
            raise RuntimeError("deterministic result does not identify schema 1.3 and its scheduler policy")
        if len(trace_bytes.splitlines()) - 1 != fixture["trace_record_count"]:
            raise RuntimeError("deterministic trace record count differs from its fixture")
        if sha256(result_bytes) != fixture["result_sha256"] or sha256(trace_bytes) != fixture["trace_sha256"]:
            raise RuntimeError("deterministic result JSON or task trace differs from its portable fixture")
        for key, value in fixture["result_fields"].items():
            actual = result
            for component in key.split("."):
                actual = actual[component]
            if actual != value:
                raise RuntimeError(f"deterministic result field {key} differs: {actual!r} != {value!r}")

        tie_first = run(root, binary, "tests/scheduler_tie_profile.conf", base / "tie-first")
        tie_second = run(root, binary, "tests/scheduler_tie_profile.conf", base / "tie-second")
        if tie_first != tie_second:
            raise RuntimeError("simultaneous cross-frame dependency fixture is not repeatable")
        tie_trace = list(csv_row for csv_row in tie_first[2].decode().splitlines()[1:])
        if not any(",profile_npu:join,NPU," in row for row in tie_trace):
            raise RuntimeError("simultaneous dependency-release fixture did not execute its join task")

    print("PASS deterministic_v1: exact result and 720-task trace, repeated output, simultaneous dependency fixture")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"deterministic scheduler contract failed: {error}", file=sys.stderr)
        raise SystemExit(1)
