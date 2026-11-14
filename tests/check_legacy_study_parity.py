#!/usr/bin/env python3
"""Replay immutable M0 CLI references and require byte-level compatibility."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def comparable_file_bytes(data: bytes) -> bytes:
    # Windows text-mode streams/files use CRLF. Normalize only that platform
    # newline translation; every other byte remains under the M0 hash check.
    if os.name == "nt":
        if b"\r" in data.replace(b"\r\n", b""):
            raise RuntimeError("unexpected bare carriage return in simulator output")
        return data.replace(b"\r\n", b"\n")
    return data


def run_case(root: Path, binary: Path, case: dict[str, object], run_number: int,
             compare_fixture: bool = True) -> tuple[bytes, bytes, bytes]:
    with tempfile.TemporaryDirectory(prefix=f"axissim-m0-{run_number}-") as directory:
        temporary = Path(directory)
        json_path = temporary / "result.json"
        trace_path = temporary / "trace.csv"
        argv = [str(binary)] + [str(item) for item in case["command_argv"][1:]]
        argv = [item.replace("<RESULT_JSON>", str(json_path)).replace("<TRACE_CSV>", str(trace_path)) for item in argv]
        completed = subprocess.run(argv, cwd=root, capture_output=True, text=True, check=False)

        if completed.returncode != case["exit_status"]:
            raise RuntimeError(
                f"{argv[1]} exit status {completed.returncode}; expected {case['exit_status']}\n"
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
            )
        if completed.stderr:
            raise RuntimeError(f"unexpected stderr from {argv[1]}:\n{completed.stderr}")

        normalized_stdout = completed.stdout.replace(str(trace_path), "<TRACE_CSV>").replace(
            str(json_path), "<RESULT_JSON>"
        ).encode()
        result_bytes = json_path.read_bytes()
        trace_bytes = trace_path.read_bytes()
        comparable_result = comparable_file_bytes(result_bytes)
        comparable_trace = comparable_file_bytes(trace_bytes)
        result = json.loads(result_bytes)

        if compare_fixture and normalized_stdout.decode() != case["stdout_normalized"]:
            raise RuntimeError(f"{argv[1]} normalized CLI output differs from the M0 fixture")
        if compare_fixture and result != case["result_json"]:
            raise RuntimeError(f"{argv[1]} schema {result.get('schema_version')} JSON values differ from M0")
        if result.get("schema_version") != case["result_schema_version"]:
            raise RuntimeError(f"{argv[1]} result schema version changed")
        if compare_fixture and (len(comparable_result) != case["result_json_bytes"] or sha256(comparable_result) != case["result_json_sha256"]):
            raise RuntimeError(f"{argv[1]} JSON bytes/hash differ from M0")
        if compare_fixture and (len(comparable_trace) != case["trace_bytes"] or sha256(comparable_trace) != case["trace_sha256"]):
            raise RuntimeError(f"{argv[1]} trace bytes/hash differ from M0")

        trace_lines = comparable_trace.splitlines()
        if not trace_lines or (compare_fixture and trace_lines[0].decode() != case["trace_header"]):
            raise RuntimeError(f"{argv[1]} trace header differs from M0")
        if compare_fixture and len(trace_lines) - 1 != case["trace_record_count"]:
            raise RuntimeError(f"{argv[1]} trace record count differs from M0")

        return normalized_stdout, comparable_result, comparable_trace


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--repeat-only", action="store_true",
                        help="test repeatability of legacy behavior on this platform without comparing it to the macOS M0 fixture")
    arguments = parser.parse_args()

    if sys.version_info < (3, 10):
        parser.error("legacy study parity check requires Python 3.10 or newer")

    root = arguments.source_root.resolve()
    binary = arguments.binary.resolve()
    fixture_path = arguments.fixture.resolve()
    fixture = json.loads(fixture_path.read_text())
    if fixture.get("schema_id") != "axissim.m0-baseline-preservation.v1":
        raise RuntimeError(f"unsupported baseline fixture schema: {fixture.get('schema_id')!r}")
    if not binary.is_file():
        raise RuntimeError(f"simulator binary does not exist: {binary}")

    for name, case in sorted(fixture["cli_baselines"].items()):
        first = run_case(root, binary, case, 1, compare_fixture=not arguments.repeat_only)
        second = run_case(root, binary, case, 2, compare_fixture=not arguments.repeat_only)
        if first != second:
            raise RuntimeError(f"{name} is not repeatable across independent output directories")
        if arguments.repeat_only:
            print(f"PASS {name}: legacy output is repeatable within this platform (not compared to the macOS M0 snapshot)")
        else:
            print(f"PASS {name}: exact M0 stdout, schema 1.2 JSON, trace, and repeatability")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"legacy study parity failed: {error}", file=sys.stderr)
        raise SystemExit(1)
