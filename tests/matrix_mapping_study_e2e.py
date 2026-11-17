#!/usr/bin/env python3
"""Real-C++ M6.3c/M6.3d execution and deterministic replay tests."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")


def refresh_bundle_hashes(root: Path) -> None:
    excluded = {"canonical-artifact-manifest.json", "host-diagnostics.jsonl", "bundle-index.json"}
    artifacts = []
    for path in sorted((item for item in root.rglob("*") if item.is_file()), key=lambda item: item.relative_to(root).as_posix()):
        rel = path.relative_to(root).as_posix()
        if rel in excluded:
            continue
        raw = path.read_bytes()
        artifacts.append({"path": rel, "size_bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    artifact_manifest = {
        "schema_id": "axissim.gemm-pair-study.artifacts", "schema_version": 1, "artifacts": artifacts,
    }
    artifact_raw = canonical(artifact_manifest)
    (root / "canonical-artifact-manifest.json").write_bytes(artifact_raw)
    diagnostic_raw = (root / "host-diagnostics.jsonl").read_bytes()
    index = {
        "schema_id": "axissim.gemm-pair-study.bundle-index", "schema_version": 1,
        "canonical_artifact_manifest_sha256": hashlib.sha256(artifact_raw).hexdigest(),
        "host_diagnostics_sha256": hashlib.sha256(diagnostic_raw).hexdigest(),
    }
    (root / "bundle-index.json").write_bytes(canonical(index))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--m6-3c-reference-bundle")
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

        manifest_v2 = source_root / "experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json"
        v2_bundle = root / "serialized-sensitivity"
        v2_replay = root / "serialized-sensitivity-replay"
        run(common + ["plan", "--manifest", str(manifest_v2), "--output-dir", str(v2_bundle)])
        v2_preflight = run(common + ["preflight", "--manifest", str(manifest_v2), "--binary", str(binary),
                                     "--output-dir", str(v2_bundle)])
        if "12 pilot cases complete" not in v2_preflight.stdout:
            raise AssertionError(f"preflight did not complete all twelve P4 pilots: {v2_preflight.stdout}")
        v2_execute = run(common + ["execute", "--manifest", str(manifest_v2), "--binary", str(binary),
                                   "--output-dir", str(v2_bundle)])
        if "Completed pairs: 24" not in v2_execute.stdout or "Actual evaluator invocations: 24" not in v2_execute.stdout:
            raise AssertionError(f"execution did not complete the approved 24-case set: {v2_execute.stdout}")
        run(common + ["verify", "--bundle", str(v2_bundle)])

        v2_preflight_record = json.loads((v2_bundle / "preflight.json").read_text(encoding="ascii"))
        if (v2_preflight_record["status"] != "ready" or v2_preflight_record["source_row_count"] != 24 or
                v2_preflight_record["unique_candidate_count"] != 24 or len(v2_preflight_record["pilot_row_ids"]) != 12):
            raise AssertionError("M6.3d preflight did not preserve the locked 24/12 case counts")
        if (v2_preflight_record["worst_case_evaluator_seconds"] != 720 or
                v2_preflight_record["reserved_peak_bytes"] > 2_097_152):
            raise AssertionError("M6.3d preflight exceeds its fixed time or storage bound")
        v2_summary = json.loads((v2_bundle / "summary.json").read_text(encoding="ascii"))
        if v2_summary["study_status"] != "complete" or v2_summary["counts"]["complete"] != 24:
            raise AssertionError("M6.3d summary is not a complete 24-candidate study")
        expected_hypotheses = {f"H{i}": "criterion_met" for i in range(1, 6)}
        actual_hypotheses = {item["hypothesis_id"]: item["status"] for item in v2_summary["hypotheses"]}
        if actual_hypotheses != expected_hypotheses:
            raise AssertionError(f"M6.3d hypothesis results differ from observed model outcomes: {actual_hypotheses}")
        v2_rows = v2_summary["rows"]
        if len(v2_rows) != 24 or any(row["runner_status"] != "complete" for row in v2_rows):
            raise AssertionError("M6.3d summary omitted or misclassified a planned candidate")
        v2_ledger = [json.loads(line) for line in (v2_bundle / "runner-ledger.jsonl").read_text(encoding="ascii").splitlines()]
        ordered = sorted((row for row in v2_ledger if "execution_order" in row), key=lambda row: row["execution_order"])
        if len(ordered) != 24 or [row["execution_order"] for row in ordered] != list(range(24)):
            raise AssertionError("M6.3d evaluator execution order is not contiguous across all 24 cases")

        # Rehash a deliberately reordered pair so verification reaches the
        # exact planned-order contract instead of stopping at file integrity.
        tampered_order = root / "tampered-order"
        shutil.copytree(v2_bundle, tampered_order)
        ledger_path = tampered_order / "runner-ledger.jsonl"
        changed_ledger = [json.loads(line) for line in ledger_path.read_text(encoding="ascii").splitlines()]
        launched_rows = sorted((item for item in changed_ledger if "execution_order" in item), key=lambda item: item["execution_order"])
        first_p8, third_p8 = launched_rows[12], launched_rows[14]
        first_p8["execution_order"], third_p8["execution_order"] = third_p8["execution_order"], first_p8["execution_order"]
        ledger_path.write_bytes(b"".join(canonical(item) for item in changed_ledger))
        diagnostic_path = tampered_order / "host-diagnostics.jsonl"
        changed_diagnostics = [json.loads(line) for line in diagnostic_path.read_text(encoding="ascii").splitlines()]
        diagnostic_by_row = {item.get("study_row_id"): item for item in changed_diagnostics if item.get("record_type") == "evaluator"}
        diag_a = diagnostic_by_row[first_p8["study_row_id"]]
        diag_b = diagnostic_by_row[third_p8["study_row_id"]]
        diag_a["sequence"], diag_b["sequence"] = diag_b["sequence"], diag_a["sequence"]
        diagnostic_path.write_bytes(b"".join(canonical(item) for item in changed_diagnostics))
        refresh_bundle_hashes(tampered_order)
        rejected_order = run(common + ["verify", "--bundle", str(tampered_order)], expected=4)
        if "exact planned P4/P8 row order" not in rejected_order.stderr:
            raise AssertionError(f"v2 verifier did not reject a correctly rehashed row-order mutation: {rejected_order.stderr}")
        v2_plan = json.loads((v2_bundle / "plan.json").read_text(encoding="ascii"))
        plan_rows = v2_plan["rows"]
        if ([row["capacity_level_id"] for row in plan_rows if row["study_row_id"] in {x["study_row_id"] for x in ordered[:12]}] != ["p4"] * 12 or
                [row["capacity_level_id"] for row in plan_rows if row["study_row_id"] in {x["study_row_id"] for x in ordered[12:]}] != ["p8"] * 12):
            raise AssertionError("M6.3d P8 execution began before all P4 pilots")
        v2_csv = list(csv.DictReader((v2_bundle / "summary.csv").open(newline="", encoding="ascii")))
        if len(v2_csv) != 24 or any(row["runner_status"] != "complete" for row in v2_csv):
            raise AssertionError("M6.3d CSV does not retain all 24 complete candidates")
        v2_diag = [json.loads(line) for line in (v2_bundle / "host-diagnostics.jsonl").read_text(encoding="ascii").splitlines()]
        evaluator_diag = [row for row in v2_diag if row["record_type"] == "evaluator"]
        preflight_calls = [row for row in evaluator_diag if row["phase"] == "preflight"]
        execute_calls = [row for row in evaluator_diag if row["phase"] == "execute"]
        if len(preflight_calls) != 12 or len(execute_calls) != 12:
            raise AssertionError("M6.3d did not execute 12 pilots before 12 remaining candidates")
        if sum(row.get("phase_elapsed_ns", 0) for row in v2_diag if row["record_type"] == "phase") > 900_000_000_000:
            raise AssertionError("M6.3d active runtime exceeded its 900-second budget")
        for artifact in v2_bundle.rglob("*"):
            if artifact.is_file() and artifact.name.endswith(".result.json") and artifact.stat().st_size > 8193:
                raise AssertionError(f"M6.3d result exceeded the 8 KiB payload cap: {artifact}")
        v2_bundle_size = sum(path.stat().st_size for path in v2_bundle.rglob("*") if path.is_file())
        if v2_bundle_size > 2_097_152:
            raise AssertionError(f"M6.3d final artifact bundle exceeds 2 MiB: {v2_bundle_size}")
        for plot in ("serialized-total-cycles.svg", "serialized-cycle-breakdown.svg"):
            raw = (v2_bundle / "plots" / plot).read_bytes()
            if len(raw) > 65_536:
                raise AssertionError(f"M6.3d plot exceeds its output cap: {plot}")
            ET.fromstring(raw)

        # Hand-derived partial-P8 expectations from the design's arithmetic example.
        partial_rows = {
            row["sensitivity_case_id"]: row for row in v2_rows
            if row["shape_id"] == "partial" and row["capacity_level_id"] == "p8"
        }
        hand_expected = {
            "bw_1e9_bytes_per_second_setup_0": {"rvv": (72, 6, 60, 138), "matrix": (64, 8, 60, 132)},
            "bw_64e9_bytes_per_second_setup_0": {"rvv": (2, 6, 1, 9), "matrix": (1, 8, 1, 10)},
            "bw_1e9_bytes_per_second_setup_1": {"rvv": (84, 6, 63, 153), "matrix": (72, 8, 64, 144)},
            "bw_64e9_bytes_per_second_setup_1": {"rvv": (14, 6, 4, 24), "matrix": (9, 8, 5, 22)},
        }
        for sensitivity_id, expected_by_arm in hand_expected.items():
            row = partial_rows[sensitivity_id]
            result = json.loads((v2_bundle / row["artifact_paths"][0]).read_text(encoding="ascii"))
            for arm_name, expected in expected_by_arm.items():
                serial = result["arms"][arm_name]["serialized_backing_store"]
                actual = tuple(serial[field] for field in ("read_cycles_per_unit", "compute_cycles_per_unit",
                                                           "write_cycles_per_unit", "total_cycles_per_unit"))
                if actual != expected:
                    raise AssertionError(f"hand-derived M6.3d partial-P8 expectation failed for {sensitivity_id}/{arm_name}: {actual}")

        run(common + ["reproduce", "--manifest", str(manifest_v2), "--binary", str(binary),
                      "--reference-bundle", str(v2_bundle), "--output-dir", str(v2_replay)])
        v2_reference_manifest = json.loads((v2_bundle / "canonical-artifact-manifest.json").read_text(encoding="ascii"))
        v2_replay_manifest = json.loads((v2_replay / "canonical-artifact-manifest.json").read_text(encoding="ascii"))
        if v2_reference_manifest != v2_replay_manifest:
            raise AssertionError("M6.3d canonical artifact manifests differ across same-binary replay")
        for item in v2_reference_manifest["artifacts"]:
            first = (v2_bundle / item["path"]).read_bytes()
            second = (v2_replay / item["path"]).read_bytes()
            if first != second or hashlib.sha256(first).hexdigest() != item["sha256"]:
                raise AssertionError(f"M6.3d canonical artifact differs or fails SHA-256: {item['path']}")
        run(common + ["verify", "--bundle", str(v2_replay)])

        if args.m6_3c_reference_bundle:
            historical = Path(args.m6_3c_reference_bundle).resolve()
            run(common + ["verify", "--bundle", str(historical)])
            historical_summary = json.loads((historical / "summary.json").read_text(encoding="ascii"))
            if historical_summary["study_run_id"] != "308735741148488aa92cee059d241aa6430ff292d8ed385c3f23c59a05e31f0e":
                raise AssertionError("M6.3c reference run identity differs from the approved baseline")
            if hashlib.sha256((historical / "canonical-artifact-manifest.json").read_bytes()).hexdigest() != "490c6623867b7adf9c3f0e3db7ff5d7a78341e24286eab06e572ccac6b7c0401":
                raise AssertionError("M6.3c canonical artifact manifest differs from the approved baseline")
            historical_by_base = {(row["shape_id"], row["capacity_level_id"]): row for row in historical_summary["rows"]}
            for row in v2_rows:
                baseline = historical_by_base[(row["shape_id"], row["capacity_level_id"])]
                result = json.loads((v2_bundle / row["artifact_paths"][0]).read_text(encoding="ascii"))
                baseline_result = json.loads((historical / baseline["artifact_paths"][0]).read_text(encoding="ascii"))
                if result["workload_id"] != baseline_result["workload_id"]:
                    raise AssertionError("M6.3d changed workload identity compared with M6.3c")
                if result["candidate_pair_id"] == baseline_result["candidate_pair_id"] or result["request_sha256"] == baseline_result["request_sha256"]:
                    raise AssertionError("M6.3d sensitivity identities were incorrectly normalized to M6.3c")
                for mapping in ("rvv", "matrix"):
                    new_arm, old_arm = result["arms"][mapping], baseline_result["arms"][mapping]
                    for field in ("mapping_id", "mapping_kind", "workload_counts", "logical_traffic",
                                  "compute_cycles_per_unit", "compute_cycles_total"):
                        if new_arm[field] != old_arm[field]:
                            raise AssertionError(f"M6.3d changed accepted M6.3c {mapping}.{field} for {row['base_case_id']}")
                    operation_prefix = "vector_mac_instructions_" if mapping == "rvv" else "matrix_tile_operations_"
                    new_ops = {key: value for key, value in new_arm.items() if key.startswith(operation_prefix)}
                    old_ops = {key: value for key, value in old_arm.items() if key.startswith(operation_prefix)}
                    if new_ops != old_ops:
                        raise AssertionError(f"M6.3d changed {mapping} operation counts for {row['base_case_id']}")
            print("M6.3d field parity with accepted M6.3c: all 24 variants preserve six baseline workloads, mappings, counts, cycles, and logical bytes")
        print(f"M6.3d C++/Python study: 24/24 pairs complete; 12 P4 pilots then 12 P8; {v2_bundle_size} bundle bytes; same-binary canonical replay exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
