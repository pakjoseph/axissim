#!/usr/bin/env python3
"""Synthetic adversarial tests for M5 replay verification."""

from __future__ import annotations

import hashlib
import json
import runpy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
VERIFIER = runpy.run_path(str(ROOT / "tools" / "verify_m5_study_replay.py"))
contained_study_path = VERIFIER["contained_study_path"]
VerificationError = VERIFIER["VerificationError"]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


class StudyArtifactPathTests(unittest.TestCase):
    def test_normal_relative_path_is_resolved_inside_study(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "traces").mkdir()
            self.assertEqual(contained_study_path(root, "traces/frame.jsonl"), root.resolve() / "traces/frame.jsonl")

    def test_parent_absolute_and_windows_paths_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "study"
            root.mkdir()
            for relative in ("../outside.json", str(root.parent / "outside.json"), r"C:\outside.json", r"..\outside.json"):
                with self.subTest(relative=relative):
                    with self.assertRaises(VerificationError):
                        contained_study_path(root, relative)

    def test_symlink_escape_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            parent = Path(temp_dir)
            root = parent / "study"
            root.mkdir()
            outside = parent / "outside.json"
            outside.write_text("{}", encoding="utf-8")
            try:
                (root / "escape.json").symlink_to(outside)
            except (OSError, NotImplementedError):
                self.skipTest("symlink creation is unavailable")
            with self.assertRaisesRegex(VerificationError, "symbolic link"):
                contained_study_path(root, "escape.json")


class SyntheticReplay:
    """A tiny accepted-style pair whose summaries include complete hash inventories."""

    manifest_id = "5" * 64
    reference_build_id = "f" * 64
    replay_build_id = "e" * 64
    reference_ids = ("a" * 64, "b" * 64)
    replay_ids = ("c" * 64, "d" * 64)

    def __init__(self, base: Path):
        self.reference = base / "reference"
        self.replay = base / "replay"
        self.reference.mkdir()
        self.replay.mkdir()
        self._write_study(self.reference, self.reference_ids, self.reference_build_id)
        self._write_study(self.replay, self.replay_ids, self.replay_build_id)
        summary_sha = sha256((self.reference / "study-summary.json").read_bytes())
        self.patch = patch.dict(VERIFIER["verify"].__globals__, {
            "EXPECTED_CANDIDATES": 2,
            "EXPECTED_RETAINED_TRACES": 1,
            "EXPECTED_REFERENCE_ARTIFACTS": 7,
            "ACCEPTED_M5_MANIFEST_ID": self.manifest_id,
            "ACCEPTED_M5_SUMMARY_SHA256": summary_sha,
            "ACCEPTED_M5_SIMULATOR_ID": self.reference_build_id,
        })
        self.patch.start()

    @staticmethod
    def _write_study(root: Path, ids: tuple[str, str], build_id: str) -> None:
        for index, candidate_id in enumerate(ids):
            request = {
                "schema_id": "axissim.study-candidate-request.v1",
                "candidate_id": candidate_id,
                "simulator_build_identity": build_id,
                "configuration_identity": "1" * 64,
                "model_assumption_identity": "2" * 64,
                "configuration": {"npu_macs": 512 + 512 * index},
                "workload_id": "synthetic-gemm",
            }
            write_json(root / "requests" / f"{candidate_id}.json", request)
            result = {
                "schema_id": "axissim.study-candidate-result.v1",
                "candidate_id": candidate_id,
                "simulator_build_identity": build_id,
                "configuration_identity": "1" * 64,
                "model_assumption_identity": "2" * 64,
                "provenance": {"simulator_build_identity": "3" * 64},
                "workload_id": "synthetic-gemm",
                "metrics": {"latency_us": 100.0 + index, "energy_j": 0.01 + index * 0.001,
                            "signed_zero": 0.0},
            }
            write_json(root / "results" / f"{candidate_id}.json", result)

        trace_id = ids[0]
        trace_request = {
            "schema_id": "axissim.study-candidate-request.v1",
            "candidate_id": trace_id,
            "simulator_build_identity": build_id,
            "configuration_identity": "1" * 64,
            "model_assumption_identity": "2" * 64,
            "configuration": {"npu_macs": 512},
            "workload_id": "synthetic-gemm",
            "trace_requested": True,
        }
        write_json(root / "requests" / f"{trace_id}.trace.json", trace_request)
        rows = [
            {"schema_id": "axissim.study-task-trace.v1", "candidate_id": trace_id,
             "simulator_build_identity": build_id, "manifest_id": SyntheticReplay.manifest_id},
            {"record_type": "task", "task_sequence": 0, "task_name": "gemm", "start_us": 0.0,
             "end_us": 100.0, "energy_j": {"npu_arithmetic": 0.01}},
        ]
        trace_data = "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows).encode()
        trace_path = root / "traces" / f"{trace_id}.jsonl"
        trace_path.parent.mkdir(parents=True, exist_ok=True)
        trace_path.write_bytes(trace_data)
        write_json(root / "trace-index.json", {
            "schema_id": "axissim.study-trace-index.v1",
            "records": [{"candidate_id": trace_id, "status": "written", "reason": None,
                         "trace_path": f"traces/{trace_id}.jsonl", "record_count": 1,
                         "bytes": len(trace_data), "sha256": sha256(trace_data)}],
            "retained_trace_bytes": len(trace_data),
            "trace_cap_bytes": 10000,
        })
        SyntheticReplay.rewrite_inventory(root, build_id)

    @staticmethod
    def rewrite_inventory(root: Path, build_id: str) -> None:
        files = sorted(p for p in root.rglob("*") if p.is_file() and p.name != "study-summary.json")
        records = [{"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size,
                    "sha256": sha256(p.read_bytes())} for p in files]
        write_json(root / "study-summary.json", {
            "schema_id": "axissim.budget-constrained-study-summary.v1",
            "manifest_id": SyntheticReplay.manifest_id,
            "candidate_count_selected": 2,
            "simulator_build_identity": build_id,
            "canonical_artifacts": records,
        })


class ReplayVerifierMutationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.fixture = SyntheticReplay(Path(self.temporary.name))
        self.addCleanup(self.fixture.patch.stop)

    def verify(self) -> dict:
        return VERIFIER["verify"](self.fixture.reference, self.fixture.replay)

    def rejected(self, message: str) -> None:
        with self.assertRaisesRegex(VerificationError, message):
            self.verify()

    def test_only_documented_build_identity_fields_are_normalized(self) -> None:
        report = self.verify()
        self.assertEqual(report["candidate_requests_matched"], 2)
        self.assertEqual(report["candidate_results_equal_after_build_identity_normalization"], 2)
        self.assertEqual(report["retained_task_traces_equal_after_build_identity_normalization"], 1)
        self.assertEqual(report["retained_task_trace_rows_equal"], 1)

    def test_modified_numeric_result_is_rejected_exactly(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        result = json.loads(path.read_text(encoding="utf-8"))
        result["metrics"]["latency_us"] += 1e-12
        write_json(path, result)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("candidate result differs beyond build identity")

    def test_decimal_precision_below_binary64_resolution_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        raw = path.read_text(encoding="utf-8")
        self.assertIn('"latency_us":101.0', raw)
        path.write_text(raw.replace('"latency_us":101.0', '"latency_us":101.0000000000000001'), encoding="utf-8")
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("candidate result differs beyond build identity")

    def test_signed_zero_change_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        raw = path.read_text(encoding="utf-8")
        self.assertIn('"signed_zero":0.0', raw)
        path.write_text(raw.replace('"signed_zero":0.0', '"signed_zero":-0.0'), encoding="utf-8")
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("candidate result differs beyond build identity")

    def test_duplicate_json_key_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        raw = path.read_text(encoding="utf-8")
        self.assertIn('"latency_us":101.0', raw)
        path.write_text(raw.replace('"latency_us":101.0', '"latency_us":101.0,"latency_us":101.0'), encoding="utf-8")
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("duplicate JSON object key")

    def test_nonfinite_json_number_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        raw = path.read_text(encoding="utf-8")
        self.assertIn('"latency_us":101.0', raw)
        path.write_text(raw.replace('"latency_us":101.0', '"latency_us":NaN'), encoding="utf-8")
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("non-standard JSON number")

    def test_missing_required_result_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        (self.fixture.replay / "results" / f"{candidate_id}.json").unlink()
        self.rejected("missing canonical artifact")

    def _mutate_trace(self, remove_task_row: bool) -> None:
        candidate_id = self.fixture.replay_ids[0]
        trace_path = self.fixture.replay / "traces" / f"{candidate_id}.jsonl"
        rows = [json.loads(line) for line in trace_path.read_text(encoding="utf-8").splitlines()]
        if remove_task_row:
            rows.pop()
        else:
            # Keep the serialized byte count unchanged so the deep event comparison is exercised.
            rows[1]["start_us"] = 1.0
        trace_data = "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows).encode()
        trace_path.write_bytes(trace_data)
        index_path = self.fixture.replay / "trace-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        record = index["records"][0]
        if not remove_task_row:
            record["record_count"] = len(rows) - 1
        record["bytes"] = len(trace_data)
        record["sha256"] = sha256(trace_data)
        index["retained_trace_bytes"] = len(trace_data)
        write_json(index_path, index)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)

    def test_altered_task_trace_event_is_rejected(self) -> None:
        self._mutate_trace(remove_task_row=False)
        self.rejected("task trace differs beyond build identity")

    def test_removed_trace_record_is_rejected(self) -> None:
        self._mutate_trace(remove_task_row=True)
        self.rejected("trace record count does not match index")

    def test_modified_candidate_input_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "requests" / f"{candidate_id}.json"
        request = json.loads(path.read_text(encoding="utf-8"))
        request["configuration"]["npu_macs"] = 8192
        write_json(path, request)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("candidate request sets differ")

    def test_invalid_artifact_hash_is_rejected(self) -> None:
        path = self.fixture.replay / "study-summary.json"
        summary = json.loads(path.read_text(encoding="utf-8"))
        summary["canonical_artifacts"][0]["sha256"] = "0" * 64
        write_json(path, summary)
        self.rejected("canonical artifact integrity mismatch")

    def test_unexpected_unindexed_file_is_rejected(self) -> None:
        (self.fixture.replay / "unlisted-result.json").write_text("{}\n", encoding="utf-8")
        self.rejected("canonical artifact file set mismatch")

    def test_unsupported_identity_field_change_is_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        result = json.loads(path.read_text(encoding="utf-8"))
        result["provenance"]["simulator_build_identity"] = "9" * 64
        write_json(path, result)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("candidate result differs beyond build identity")

    def test_result_build_identity_must_match_its_study_summary(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "results" / f"{candidate_id}.json"
        result = json.loads(path.read_text(encoding="utf-8"))
        result["simulator_build_identity"] = "9" * 64
        write_json(path, result)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("replay result build identity does not match study summary")

    def test_request_build_identity_must_match_its_study_summary(self) -> None:
        candidate_id = self.fixture.replay_ids[1]
        path = self.fixture.replay / "requests" / f"{candidate_id}.json"
        request = json.loads(path.read_text(encoding="utf-8"))
        request["simulator_build_identity"] = "9" * 64
        write_json(path, request)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("request build identity does not match study summary")

    def test_trace_header_candidate_identity_must_match_its_request(self) -> None:
        candidate_id = self.fixture.replay_ids[0]
        path = self.fixture.replay / "traces" / f"{candidate_id}.jsonl"
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        rows[0]["candidate_id"] = "9" * 64
        trace_data = "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows).encode()
        path.write_bytes(trace_data)
        index_path = self.fixture.replay / "trace-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["records"][0]["bytes"] = len(trace_data)
        index["records"][0]["sha256"] = sha256(trace_data)
        write_json(index_path, index)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("trace header candidate identity does not match its request")

    def test_trace_header_build_identity_must_match_its_study_summary(self) -> None:
        candidate_id = self.fixture.replay_ids[0]
        path = self.fixture.replay / "traces" / f"{candidate_id}.jsonl"
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        rows[0]["simulator_build_identity"] = "9" * 64
        trace_data = "".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n" for row in rows).encode()
        path.write_bytes(trace_data)
        index_path = self.fixture.replay / "trace-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["records"][0]["bytes"] = len(trace_data)
        index["records"][0]["sha256"] = sha256(trace_data)
        write_json(index_path, index)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("replay trace build identity does not match study summary")

    def test_unknown_trace_index_fields_are_rejected(self) -> None:
        index_path = self.fixture.replay / "trace-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["model_assumption_identity"] = "9" * 64
        write_json(index_path, index)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("unexpected trace index fields")

    def test_unknown_trace_record_fields_are_rejected(self) -> None:
        index_path = self.fixture.replay / "trace-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["records"][0]["unmodeled_cost"] = 0
        write_json(index_path, index)
        SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
        self.rejected("unexpected trace index record fields")

    def test_trace_path_traversal_and_unexpected_location_are_rejected(self) -> None:
        candidate_id = self.fixture.replay_ids[0]
        index_path = self.fixture.replay / "trace-index.json"
        for trace_path in ("../escape.jsonl", f"results/{candidate_id}.json"):
            with self.subTest(trace_path=trace_path):
                # Restore a clean replay for each path mutation.
                SyntheticReplay._write_study(self.fixture.replay, self.fixture.replay_ids,
                                             self.fixture.replay_build_id)
                index = json.loads(index_path.read_text(encoding="utf-8"))
                index["records"][0]["trace_path"] = trace_path
                write_json(index_path, index)
                SyntheticReplay.rewrite_inventory(self.fixture.replay, self.fixture.replay_build_id)
                with self.assertRaises(VerificationError):
                    self.verify()


if __name__ == "__main__":
    unittest.main()
