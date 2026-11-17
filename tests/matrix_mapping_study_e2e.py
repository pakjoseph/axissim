#!/usr/bin/env python3
"""Real-C++ six-case execution and deterministic replay test."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    source_root = Path(args.source_root).resolve()
    binary = Path(args.binary).resolve()
    runner = source_root / "experiments/run_matrix_mapping_study.py"
    manifest = source_root / "experiments/matrix_mapping_study/manifest.v1.json"
    with tempfile.TemporaryDirectory(prefix="axissim-m63c-e2e-") as temp:
        root = Path(temp)
        bundle = root / "first"
        replay = root / "replay"
        common = [sys.executable, str(runner)]
        run(common + ["plan", "--manifest", str(manifest), "--output-dir", str(bundle)])
        preflight = run(common + ["preflight", "--manifest", str(manifest), "--binary", str(binary), "--output-dir", str(bundle)])
        if "3 pilot cases complete" not in preflight.stdout:
            raise AssertionError(f"preflight did not confirm all three P4 pilots: {preflight.stdout}")
        execute = run(common + ["execute", "--manifest", str(manifest), "--binary", str(binary), "--output-dir", str(bundle)])
        if "Completed pairs: 6" not in execute.stdout or "Actual evaluator invocations: 6" not in execute.stdout:
            raise AssertionError(f"execution did not complete the approved six-case set: {execute.stdout}")
        run(common + ["verify", "--bundle", str(bundle)])

        preflight_record = json.loads((bundle / "preflight.json").read_text(encoding="ascii"))
        if preflight_record["status"] != "ready" or preflight_record["source_row_count"] != 6 or preflight_record["unique_candidate_count"] != 6:
            raise AssertionError("preflight metadata differs from the locked study")
        if preflight_record["worst_case_evaluator_seconds"] != 180 or preflight_record["reserved_peak_bytes"] > 2_097_152:
            raise AssertionError("preflight limits are not enforced or reported correctly")
        summary = json.loads((bundle / "summary.json").read_text(encoding="ascii"))
        if summary["study_status"] != "complete" or summary["counts"]["complete"] != 6:
            raise AssertionError("six-case summary is not complete")
        if any("metrics" not in row for row in summary["rows"]):
            raise AssertionError("a complete row lacks C++-owned metrics")
        rows = list(csv.DictReader((bundle / "summary.csv").open(newline="", encoding="ascii")))
        if len(rows) != 6 or any(row["runner_status"] != "complete" for row in rows):
            raise AssertionError("CSV candidate ledger is incomplete")
        diag = [json.loads(line) for line in (bundle / "host-diagnostics.jsonl").read_text(encoding="ascii").splitlines()]
        evaluators = [row for row in diag if row["record_type"] == "evaluator"]
        if len(evaluators) != 6 or sum(row["phase"] == "preflight" for row in evaluators) != 3 or sum(row["phase"] == "execute" for row in evaluators) != 3:
            raise AssertionError("pilot and remaining-candidate invocation counts are incorrect")
        if any(not isinstance(row["stdout_bytes"], int) or row["stdout_bytes"] < 0 or
               not isinstance(row["stderr_bytes"], int) or row["stderr_bytes"] < 0 or
               len(row["stdout_sha256"]) != 64 or len(row["stderr_sha256"]) != 64 for row in evaluators):
            raise AssertionError("host diagnostic stream hashes are malformed")
        if sum(row["phase_elapsed_ns"] for row in diag if row["record_type"] == "phase") > 900_000_000_000:
            raise AssertionError("observed active time exceeded its 15-minute budget")
        size = sum(path.stat().st_size for path in bundle.rglob("*") if path.is_file())
        if size > 2_097_152:
            raise AssertionError(f"final artifact bundle exceeds 2 MiB: {size}")

        run(common + ["reproduce", "--manifest", str(manifest), "--binary", str(binary),
                      "--reference-bundle", str(bundle), "--output-dir", str(replay)])
        reference_manifest = json.loads((bundle / "canonical-artifact-manifest.json").read_text(encoding="ascii"))
        replay_manifest = json.loads((replay / "canonical-artifact-manifest.json").read_text(encoding="ascii"))
        if reference_manifest != replay_manifest:
            raise AssertionError("canonical artifact manifests differ across same-binary replay")
        for item in reference_manifest["artifacts"]:
            first = (bundle / item["path"]).read_bytes()
            second = (replay / item["path"]).read_bytes()
            if first != second or hashlib.sha256(first).hexdigest() != item["sha256"]:
                raise AssertionError(f"canonical artifact differs or fails SHA-256: {item['path']}")
        run(common + ["verify", "--bundle", str(replay)])
        print("M6.3c C++/Python study: 6/6 pairs complete; 3 P4 pilots; same-binary canonical replay exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
