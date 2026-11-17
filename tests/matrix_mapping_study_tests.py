#!/usr/bin/env python3
"""Deterministic unit and controlled-failure tests for the M6.3c runner."""
from __future__ import annotations

import argparse
import contextlib
import csv
import hashlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


def load_runner(source_root: Path):
    path = source_root / "experiments/run_matrix_mapping_study.py"
    spec = importlib.util.spec_from_file_location("axissim_matrix_mapping_runner", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load runner module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RunnerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        parser = argparse.ArgumentParser()
        parser.add_argument("--source-root", required=True)
        parser.add_argument("--binary", required=True)
        args = parser.parse_args()
        cls.source_root = Path(args.source_root).resolve()
        cls.binary = Path(args.binary).resolve()
        cls.runner = load_runner(cls.source_root)
        cls.manifest_path = cls.source_root / "experiments/matrix_mapping_study/manifest.v1.json"
        cls.manifest, cls.manifest_raw, cls.manifest_id = cls.runner._load_runner_manifest(cls.manifest_path)
        cls.rows = cls.runner.expand_candidates(cls.manifest, cls.manifest_id)
        cls.manifest_v2_path = cls.source_root / "experiments/matrix_mapping_study/manifest.v2.serialized-sensitivity.json"
        cls.manifest_v2, cls.manifest_v2_raw, cls.manifest_v2_id = cls.runner._load_runner_manifest(cls.manifest_v2_path)
        cls.rows_v2 = cls.runner.expand_candidates(cls.manifest_v2, cls.manifest_v2_id)

    def test_v2_manifest_expands_exact_24_ordered_candidates_and_twelve_pilots(self) -> None:
        r = self.runner
        self.assertEqual(r.sha256_bytes(self.manifest_raw), r.M63C_V1_MANIFEST_SHA256)
        self.assertEqual(self.manifest_v2["schema_version"], 2)
        self.assertEqual(len(self.rows_v2), 24)
        self.assertEqual(len({row["request_bytes"] for row in self.rows_v2}), 24)
        self.assertEqual(len({row["candidate_pair_id"] for row in self.rows_v2}), 24)
        sensitivity_order = [case["sensitivity_case_id"] for case in r.V2_SENSITIVITY_CASES]
        expected_bases = ["tiny_p4", "tiny_p8", "aligned_p4", "aligned_p8", "partial_p4", "partial_p8"]
        self.assertEqual([row["base_case_id"] for row in self.rows_v2], [base for base in expected_bases for _ in sensitivity_order])
        self.assertEqual([row["sensitivity_case_id"] for row in self.rows_v2[:4]], sensitivity_order)
        pilots = r._pilot_candidates(self.rows_v2, self.manifest_v2)
        self.assertEqual(len(pilots), 12)
        self.assertEqual([row["capacity_level_id"] for row in pilots], ["p4"] * 12)
        self.assertEqual([row["source_row_index"] for row in pilots], [0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19])
        for base in expected_bases:
            variants = [row for row in self.rows_v2 if row["base_case_id"] == base]
            stripped = []
            for row in variants:
                request = json.loads(row["request_bytes"])
                del request["measurement"]["backing_store_sensitivity"]
                stripped.append(r.canonical_bytes(request))
            self.assertEqual(len(set(stripped)), 1, base)
            self.assertEqual(len({row["ids"]["request_sha256"] for row in variants}), 4)
            self.assertEqual(len({row["ids"]["model_assumptions_sha256"] for row in variants}), 4)

    def test_v2_manifest_rejects_sensitivity_and_resource_limit_mutations(self) -> None:
        r = self.runner
        for mutate in (
            lambda value: value["sensitivity_cases"][0].__setitem__("bandwidth_bytes_per_second", 2_000_000_000),
            lambda value: value["sensitivity_cases"].reverse(),
            lambda value: value["limits"].__setitem__("max_result_payload_bytes", 65536),
            lambda value: value["baseline_reference"].__setitem__("study_run_id", "0" * 64),
            lambda value: value["shapes"][0].__setitem__("m", 3),
            lambda value: value["workload"].__setitem__("batch_count", True),
            lambda value: value["measurement"].__setitem__("repeat_count", True),
            lambda value: value["mapping_constants"]["matrix"].__setitem__("kernel_setup_cycles", False),
        ):
            changed = json.loads(json.dumps(self.manifest_v2))
            mutate(changed)
            with self.assertRaises(r.RunnerError):
                r.validate_manifest(changed)

    def test_v2_cxx_result_identity_and_compute_only_parity_across_sensitivities(self) -> None:
        r = self.runner
        variants = [row for row in self.rows_v2 if row["base_case_id"] == "partial_p8"]
        records = []
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-identity-") as temp:
            root = Path(temp)
            for index, row in enumerate(variants):
                request = root / f"request-{index}.json"
                result_path = root / f"result-{index}.json"
                request.write_bytes(row["request_bytes"])
                proc = r.invoke_evaluator(self.binary, request, result_path, 10)
                self.assertEqual(proc["process_exit_code"], 0)
                result, raw, error = r._cpp_response(result_path, 8192)
                self.assertIsNone(error)
                self.assertLessEqual(len(raw), 8193)
                valid, reason = r.validate_candidate_result(result, row, r._file_hash(self.binary))
                self.assertTrue(valid, reason)
                records.append(result)
            self.assertEqual(len({item["candidate_pair_id"] for item in records}), 4)
            self.assertEqual(len({item["request_sha256"] for item in records}), 4)
            self.assertEqual(len({item["model_assumptions_sha256"] for item in records}), 4)
            first = records[0]
            for record in records[1:]:
                self.assertEqual(record["workload_id"], first["workload_id"])
                for mapping in ("rvv", "matrix"):
                    for field in ("mapping_id", "mapping_kind", "workload_counts", "logical_traffic", "compute_cycles_per_unit", "compute_cycles_total"):
                        self.assertEqual(record["arms"][mapping][field], first["arms"][mapping][field])
                    serial = record["arms"][mapping]["serialized_backing_store"]
                    self.assertEqual(serial["compute_cycles_per_unit"], record["arms"][mapping]["compute_cycles_per_unit"])
                    self.assertEqual(serial["total_cycles_per_unit"], sum(serial[key] for key in ("read_cycles_per_unit", "compute_cycles_per_unit", "write_cycles_per_unit")))
            bad = json.loads(json.dumps(first))
            bad["measurement_protocol"]["backing_store_sensitivity"]["bandwidth_bytes_per_second"] += 1
            self.assertEqual(r.validate_candidate_result(bad, variants[0], r._file_hash(self.binary)), (False, "result_schema_error"))
            bad = json.loads(json.dumps(first))
            del bad["arms"]["rvv"]["serialized_backing_store"]["read_cycles_per_unit"]
            self.assertEqual(r.validate_candidate_result(bad, variants[0], r._file_hash(self.binary)), (False, "result_schema_error"))

            # The v2 result contract must reject malformed serial objects and
            # identity/status corruption independently of the v1 mutation suite.
            invalid_cases = (
                (lambda value: value["arms"]["rvv"]["serialized_backing_store"].__setitem__("unexpected", 0), "result_schema_error"),
                (lambda value: value["arms"]["rvv"]["serialized_backing_store"].__setitem__("read_cycles_per_unit", True), "result_schema_error"),
                (lambda value: value.__setitem__("candidate_pair_id", "0" * 64), "identity_mismatch"),
                (lambda value: value.__setitem__("workload_id", "0" * 64), "identity_mismatch"),
                (lambda value: value["arms"]["rvv"].__setitem__("mapping_id", "0" * 64), "identity_mismatch"),
                (lambda value: value["arms"]["matrix"].__setitem__("mapping_id", "0" * 64), "identity_mismatch"),
                (lambda value: value["measurement_protocol"]["backing_store_sensitivity"].__setitem__("batch_setup_cycles", True), "result_schema_error"),
                (lambda value: value.__setitem__("status", "partial"), "result_schema_error"),
                (lambda value: value["arms"]["matrix"].__setitem__("arm_status", "failed"), "result_schema_error"),
            )
            for mutate, expected_reason in invalid_cases:
                bad = json.loads(json.dumps(first))
                mutate(bad)
                self.assertEqual(r.validate_candidate_result(bad, variants[0], r._file_hash(self.binary)), (False, expected_reason))

            ledger = {
                "process_exit_code": 0, "candidate_pair_id": variants[0]["candidate_pair_id"],
                "cpp_status": "complete", "rvv_arm_status": "complete", "matrix_arm_status": "complete",
                "error_codes": [], "error_paths": [], "execution_order": 0,
            }
            r._verify_result_ledger_consistency("complete", ledger, first, variants[0])
            corrupt_ledger = dict(ledger, error_codes=["tampered"])
            with self.assertRaises(r.RunnerError):
                r._verify_result_ledger_consistency("complete", corrupt_ledger, first, variants[0])

            # The aligned fixture can have equal compute counts while the two
            # mappings still produce different logical payload and service.
            aligned = next(row for row in self.rows_v2 if row["base_case_id"] == "aligned_p4" and
                           row["sensitivity_case_id"] == "bw_1e9_bytes_per_second_setup_0")
            request = root / "request-aligned.json"
            result_path = root / "result-aligned.json"
            request.write_bytes(aligned["request_bytes"])
            proc = r.invoke_evaluator(self.binary, request, result_path, 10)
            self.assertEqual(proc["process_exit_code"], 0)
            aligned_record, _, error = r._cpp_response(result_path, 8192)
            self.assertIsNone(error)
            valid, reason = r.validate_candidate_result(aligned_record, aligned, r._file_hash(self.binary))
            self.assertTrue(valid, reason)
            aligned_rvv, aligned_matrix = aligned_record["arms"]["rvv"], aligned_record["arms"]["matrix"]
            self.assertEqual(aligned_rvv["compute_cycles_per_unit"], aligned_matrix["compute_cycles_per_unit"])
            self.assertNotEqual(aligned_rvv["logical_traffic"], aligned_matrix["logical_traffic"])
            self.assertNotEqual(
                aligned_rvv["serialized_backing_store"]["read_cycles_per_unit"] + aligned_rvv["serialized_backing_store"]["write_cycles_per_unit"],
                aligned_matrix["serialized_backing_store"]["read_cycles_per_unit"] + aligned_matrix["serialized_backing_store"]["write_cycles_per_unit"],
            )

    def test_v2_hypothesis_rules_use_only_complete_pairs(self) -> None:
        r = self.runner
        synthetic: dict[int, dict[str, Any]] = {}
        for row in self.rows_v2:
            request = row["request"]
            sensitivity = request["measurement"]["backing_store_sensitivity"]
            setup = sensitivity["batch_setup_cycles"]
            high_bandwidth = sensitivity["bandwidth_bytes_per_second"] == 64_000_000_000
            arms = {}
            for mapping, compute, traffic, read0, write0 in (
                ("rvv", 10 if row["capacity_level_id"] == "p4" else 6,
                 {"read_a_bytes_per_unit": 12, "read_b_bytes_per_unit": 60, "write_c_bytes_per_unit": 60,
                  "total_bytes_per_unit": 132, "read_a_batches_per_unit": 6, "read_b_batches_per_unit": 6, "write_c_batches_per_unit": 3}, 10, 5),
                ("matrix", 12 if row["capacity_level_id"] == "p4" else 8,
                 {"read_a_bytes_per_unit": 24, "read_b_bytes_per_unit": 40, "write_c_bytes_per_unit": 60,
                  "total_bytes_per_unit": 124, "read_a_batches_per_unit": 4, "read_b_batches_per_unit": 4, "write_c_batches_per_unit": 4}, 2, 1),
            ):
                read = (read0 // 3 if high_bandwidth else read0) + setup * (traffic["read_a_batches_per_unit"] + traffic["read_b_batches_per_unit"])
                write = (write0 // 2 if high_bandwidth else write0) + setup * traffic["write_c_batches_per_unit"]
                arms[mapping] = {
                    "compute_cycles_per_unit": compute,
                    "logical_traffic": traffic,
                    "serialized_backing_store": {
                        "read_cycles_per_unit": read, "compute_cycles_per_unit": compute,
                        "write_cycles_per_unit": write, "total_cycles_per_unit": read + compute + write,
                    },
                }
            synthetic[row["source_row_index"]] = {"runner_status": "complete", "record": {"arms": arms}}
        outcomes = {item["hypothesis_id"]: item for item in r._hypothesis_results_v2(self.manifest_v2, self.rows_v2, synthetic)}
        self.assertTrue(all(outcomes[key]["status"] == "criterion_met" for key in ("H1", "H2", "H3", "H4", "H5")))
        findings = "\n".join(r._v2_observed_findings(self.manifest_v2, self.rows_v2, synthetic))
        for label in ("H1 — mapping traffic and service", "H2 — P4 to P8", "H3 — declared bandwidth",
                      "H4 — setup cycles", "H5 — ranking changes"):
            self.assertIn(label, findings)
        self.assertIn("serialized", findings.lower())
        incomplete = dict(synthetic)
        incomplete.pop(23)
        outcomes = {item["hypothesis_id"]: item for item in r._hypothesis_results_v2(self.manifest_v2, self.rows_v2, incomplete)}
        self.assertEqual(outcomes["H3"]["status"], "inconclusive")
        self.assertEqual(outcomes["H4"]["status"], "inconclusive")
        incomplete[0] = {"runner_status": "partial", "record": synthetic[0]["record"]}
        self.assertEqual(r._v2_complete_record(self.rows_v2[0], incomplete), None)

    def test_v2_pilot_gate_runs_all_p4_then_prevents_any_p8_on_failure(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-pilot-gate-") as temp:
            root = Path(temp) / "bundle"
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            launched = []

            def failed_evaluator(binary, request_path, result_path, timeout):
                request = r.parse_json(request_path.read_bytes().rstrip(b"\n"))
                launched.append(request)
                return {"process_exit_code": 1, "timed_out": False, "cancelled": False, "launch_error": False,
                        "elapsed_ns": 100, "stdout_bytes": 0, "stdout_sha256": r.sha256_bytes(b""),
                        "stderr_bytes": 0, "stderr_sha256": r.sha256_bytes(b"")}

            with mock.patch.object(r, "invoke_evaluator", side_effect=failed_evaluator):
                self.assertEqual(r.command_preflight(args), 3)
            self.assertEqual(len(launched), 12)
            self.assertTrue(all(request["mappings"]["matrix"]["mac_slots_per_cycle"] == 4 for request in launched))
            self.assertEqual(r.verify_bundle(root)["status"], "verified")
            preflight, _ = r.read_json(root / "preflight.json")
            self.assertEqual(preflight["status"], "pilot_failed")
            ledger, _ = r.read_jsonl(root / "runner-ledger.jsonl")
            self.assertEqual(sum("execution_order" in row for row in ledger), 12)
            self.assertTrue(all(row["runner_status"] == "not_started_due_to_preflight_failure" for row in ledger if row["base_case_id"].endswith("_p8")))

    def test_v2_bundle_launch_order_must_be_the_exact_planned_prefix(self) -> None:
        r = self.runner
        pilots = [row for row in self.rows_v2 if row["capacity_level_id"] == "p4"]
        p8 = [row for row in self.rows_v2 if row["capacity_level_id"] == "p8"]
        expected = pilots + p8
        # Build a row-aligned ledger whose order maps to the first 12 P4 rows,
        # then the first 3 P8 rows as allowed after a successful pilot gate.
        launch_ids = [row["source_row_index"] for row in expected[:15]]
        aligned = [{} for _ in self.rows_v2]
        for order, row_index in enumerate(launch_ids):
            aligned[row_index]["execution_order"] = order
        self.assertTrue(r._v2_launch_order_matches(self.rows_v2, aligned))
        aligned[4]["execution_order"], aligned[6]["execution_order"] = 14, 12
        self.assertFalse(r._v2_launch_order_matches(self.rows_v2, aligned))

    def test_v2_active_budget_includes_candidate_orchestration_and_publication(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-active-budget-") as temp:
            root = Path(temp) / "bundle"
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            self.assertEqual(r.command_preflight(args), 0)
            diagnostics, _ = r.read_jsonl(root / "host-diagnostics.jsonl")
            phase = next(item for item in diagnostics if item.get("record_type") == "phase" and item.get("phase") == "preflight")
            phase["phase_elapsed_ns"] = 899_000_000_000
            (root / "host-diagnostics.jsonl").write_bytes(r._jsonl_bytes(diagnostics))

            now = [0]
            def tick() -> int:
                now[0] += 100_000_000
                return now[0]

            real_invoke = r._invoke_candidate
            def costly_invoke(*call_args):
                result = real_invoke(*call_args)
                now[0] += 300_000_000
                return result

            with mock.patch.object(r.time, "monotonic_ns", side_effect=tick), \
                 mock.patch.object(r, "_invoke_candidate", side_effect=costly_invoke) as invoke:
                code = r.command_execute(args)
            self.assertEqual(code, 3)
            self.assertGreater(invoke.call_count, 0)
            self.assertLess(invoke.call_count, 12)
            verified = r.verify_bundle(root)
            self.assertEqual(verified["status"], "verified")
            summary, _ = r.read_json(root / "summary.json")
            self.assertEqual(summary["study_status"], "limit_reached")
            host, _ = r.read_jsonl(root / "host-diagnostics.jsonl")
            recorded = sum(item.get("phase_elapsed_ns", 0) for item in host if item.get("record_type") == "phase")
            self.assertGreater(recorded, r.V2_LIMITS["max_active_seconds"] * 1_000_000_000)

    def test_v2_post_report_publication_overrun_marks_completed_study_limited(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-publication-overrun-") as temp:
            root = Path(temp) / "bundle"
            cache = Path(temp) / "cache"
            cache.mkdir()
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            self.assertEqual(r.command_preflight(args), 0)
            p8_rows = [row for row in self.rows_v2 if row["capacity_level_id"] == "p8"]
            result_by_request: dict[str, bytes] = {}
            for index, row in enumerate(p8_rows):
                result_path = cache / f"p8-{index}.json"
                proc = r.invoke_evaluator(self.binary, root / row["request_path"], result_path, 10)
                self.assertEqual(proc["process_exit_code"], 0)
                result, raw, error = r._cpp_response(result_path, 8192)
                self.assertIsNone(error)
                valid, reason = r.validate_candidate_result(result, row, r._file_hash(self.binary))
                self.assertTrue(valid, reason)
                result_by_request[row["request_sha256"]] = raw

            diagnostics, _ = r.read_jsonl(root / "host-diagnostics.jsonl")
            phase = next(item for item in diagnostics if item.get("record_type") == "phase" and item.get("phase") == "preflight")
            phase["phase_elapsed_ns"] = 899_000_000_000
            (root / "host-diagnostics.jsonl").write_bytes(r._jsonl_bytes(diagnostics))

            def cached_evaluate(binary, request_root, row, timeout):
                raw = result_by_request[row["request_sha256"]]
                temp_name = r.hashlib.sha256(("cpp-result:" + row["study_row_id"]).encode("ascii")).hexdigest() + ".json"
                staged = request_root / ".staging" / temp_name
                staged.write_bytes(raw)
                response, parsed_raw, parse_error = r._cpp_response(staged, 8192)
                staged.unlink()
                return ({"process_exit_code": 0, "timed_out": False, "cancelled": False,
                         "launch_error": False, "elapsed_ns": 0, "stdout_bytes": 0,
                         "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                         "stderr_sha256": r.sha256_bytes(b"")}, response, parsed_raw, parse_error)

            ticks = [0] + [0] * 12 + [900_000_000, 1_100_000_000, 1_100_000_000, 1_200_000_000]
            tick_index = [0]
            def clock_tick() -> int:
                value = ticks[min(tick_index[0], len(ticks) - 1)]
                tick_index[0] += 1
                return value

            with mock.patch.object(r.time, "monotonic_ns", side_effect=clock_tick), \
                 mock.patch.object(r, "_invoke_candidate", side_effect=cached_evaluate) as invoke:
                code = r.command_execute(args)
            self.assertEqual(code, 3)
            self.assertEqual(invoke.call_count, 12)
            self.assertEqual(r.verify_bundle(root)["status"], "verified")
            summary, _ = r.read_json(root / "summary.json")
            self.assertEqual(summary["study_status"], "limit_reached")
            self.assertEqual(summary["counts"]["complete"], 24)

    def test_v2_final_seal_overrun_marks_completed_study_limited(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-final-seal-overrun-") as temp:
            root = Path(temp) / "bundle"
            cache = Path(temp) / "cache"
            cache.mkdir()
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            self.assertEqual(r.command_preflight(args), 0)
            p8_rows = [row for row in self.rows_v2 if row["capacity_level_id"] == "p8"]
            result_by_request: dict[str, bytes] = {}
            for index, row in enumerate(p8_rows):
                result_path = cache / f"p8-{index}.json"
                proc = r.invoke_evaluator(self.binary, root / row["request_path"], result_path, 10)
                self.assertEqual(proc["process_exit_code"], 0)
                result, raw, error = r._cpp_response(result_path, 8192)
                self.assertIsNone(error)
                valid, reason = r.validate_candidate_result(result, row, r._file_hash(self.binary))
                self.assertTrue(valid, reason)
                result_by_request[row["request_sha256"]] = raw

            diagnostics, _ = r.read_jsonl(root / "host-diagnostics.jsonl")
            phase = next(item for item in diagnostics if item.get("record_type") == "phase" and item.get("phase") == "preflight")
            phase["phase_elapsed_ns"] = 899_900_000_000
            store = r.ArtifactStore(root)
            r._append_host_diagnostics(store, root, diagnostics[:-1], [diagnostics[-1]])

            now = [0]
            def clock_tick() -> int:
                return now[0]

            def cached_evaluate(binary, request_root, row, timeout):
                raw = result_by_request[row["request_sha256"]]
                temp_name = r.hashlib.sha256(("cpp-result:" + row["study_row_id"]).encode("ascii")).hexdigest() + ".json"
                staged = request_root / ".staging" / temp_name
                staged.write_bytes(raw)
                response, parsed_raw, parse_error = r._cpp_response(staged, 8192)
                staged.unlink()
                return ({"process_exit_code": 0, "timed_out": False, "cancelled": False,
                         "launch_error": False, "elapsed_ns": 0, "stdout_bytes": 0,
                         "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                         "stderr_sha256": r.sha256_bytes(b"")}, response, parsed_raw, parse_error)

            real_verify = r.verify_bundle
            verify_calls = [0]
            def verify_with_final_tail(root_path):
                verify_calls[0] += 1
                if verify_calls[0] == 2:
                    now[0] = 200_000_000
                return real_verify(root_path)

            with mock.patch.object(r.time, "monotonic_ns", side_effect=clock_tick), \
                 mock.patch.object(r, "_invoke_candidate", side_effect=cached_evaluate) as invoke, \
                 mock.patch.object(r, "verify_bundle", side_effect=verify_with_final_tail):
                code = r.command_execute(args)
            self.assertEqual(code, 3)
            self.assertEqual(invoke.call_count, 12)
            self.assertGreaterEqual(verify_calls[0], 3)
            verified = real_verify(root)
            self.assertEqual(verified["status"], "verified")
            summary, _ = r.read_json(root / "summary.json")
            self.assertEqual(summary["study_status"], "limit_reached")
            self.assertEqual(summary["counts"]["complete"], 24)

    def test_v2_invalid_cpp_pilot_envelopes_are_retained_and_verified(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-invalid-pilot-") as temp:
            root = Path(temp) / "bundle"
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            error = {
                "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
                "status": "invalid_request", "errors": [
                    {"scope": "request", "code": "invalid_value", "path": "/workload/m"}
                ],
            }
            raw = r.canonical_bytes(error) + b"\n"
            launched: list[dict[str, object]] = []

            def invalid_evaluator(binary, request_path, result_path, timeout):
                request = r.parse_json(Path(request_path).read_bytes().rstrip(b"\n"))
                launched.append(request)
                Path(result_path).write_bytes(raw)
                return {"process_exit_code": 2, "timed_out": False, "cancelled": False, "launch_error": False,
                        "elapsed_ns": 100, "stdout_bytes": 0, "stdout_sha256": r.sha256_bytes(b""),
                        "stderr_bytes": 0, "stderr_sha256": r.sha256_bytes(b"")}

            with mock.patch.object(r, "invoke_evaluator", side_effect=invalid_evaluator):
                self.assertEqual(r.command_preflight(args), 3)
            self.assertEqual(len(launched), 12)
            self.assertTrue(all(request["mappings"]["matrix"]["mac_slots_per_cycle"] == 4 for request in launched))
            self.assertEqual(r.verify_bundle(root)["status"], "verified")
            preflight, _ = r.read_json(root / "preflight.json")
            ledger, _ = r.read_jsonl(root / "runner-ledger.jsonl")
            self.assertEqual(preflight["status"], "pilot_failed")
            p4 = [item for item in ledger if item["base_case_id"].endswith("_p4")]
            p8 = [item for item in ledger if item["base_case_id"].endswith("_p8")]
            self.assertEqual(len(p4), 12)
            self.assertTrue(all(item["runner_status"] == "invalid" for item in p4))
            self.assertTrue(all(item["runner_status"] == "not_started_due_to_preflight_failure" for item in p8))
            for item in p4:
                error_path = Path(root / item["artifact_paths"][-1])
                self.assertEqual(error_path.read_bytes(), raw)

    def test_v2_terminal_preflight_publication_overrun_is_recorded_and_rehashed(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-preflight-overrun-") as temp:
            root = Path(temp) / "bundle"
            args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
            self.assertEqual(r.command_plan(args), 0)
            error = {
                "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
                "status": "invalid_request", "errors": [
                    {"scope": "request", "code": "controlled_invalid", "path": "/workload/m"}
                ],
            }
            error_bytes = r.canonical_bytes(error) + b"\n"
            launched = []

            def invalid_evaluator(binary, request_path, result_path, timeout):
                launched.append(Path(request_path).read_bytes())
                Path(result_path).write_bytes(error_bytes)
                return {"process_exit_code": 2, "timed_out": False, "cancelled": False,
                        "launch_error": False, "elapsed_ns": 1, "stdout_bytes": 0,
                        "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                        "stderr_sha256": r.sha256_bytes(b"")}

            ticks = [0] * 14 + [901_000_000_000, 902_000_000_000]
            tick_index = [0]
            def clock_tick() -> int:
                value = ticks[min(tick_index[0], len(ticks) - 1)]
                tick_index[0] += 1
                return value

            with mock.patch.object(r.time, "monotonic_ns", side_effect=clock_tick), \
                 mock.patch.object(r, "invoke_evaluator", side_effect=invalid_evaluator):
                self.assertEqual(r.command_preflight(args), 3)
            self.assertEqual(len(launched), 12)
            self.assertTrue(all(r.parse_json(raw.rstrip(b"\n"))["mappings"]["matrix"]["mac_slots_per_cycle"] == 4 for raw in launched))
            self.assertEqual(r.verify_bundle(root)["status"], "verified")
            preflight, _ = r.read_json(root / "preflight.json")
            summary, _ = r.read_json(root / "summary.json")
            self.assertEqual(preflight["status"], "limit_reached")
            self.assertEqual(summary["study_status"], "limit_reached")
            ledger, _ = r.read_jsonl(root / "runner-ledger.jsonl")
            self.assertTrue(all(row["runner_status"] == "not_started_due_to_limit" for row in ledger if row["base_case_id"].endswith("_p8")))

    def test_v2_terminal_partial_failed_timeout_and_output_limit_bundles_verify(self) -> None:
        r = self.runner
        target = self.rows_v2[0]
        real_invoke = r.invoke_evaluator
        cases = (
            ("partial", "partial", 3),
            ("failed", "failed", 4),
            ("timeout", "timeout", 143),
            ("output_limit", "output_limit", 5),
        )
        for mode, expected_status, exit_code in cases:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(prefix=f"axissim-m63d-terminal-{mode}-") as temp:
                root = Path(temp) / "bundle"
                args = argparse.Namespace(manifest=str(self.manifest_v2_path), binary=str(self.binary), output_dir=str(root))
                self.assertEqual(r.command_plan(args), 0)

                def controlled(binary, request_path, result_path, timeout):
                    request_raw = Path(request_path).read_bytes().rstrip(b"\n")
                    request_hash = r.sha256_bytes(request_raw)
                    if request_hash == target["request_sha256"] and mode == "timeout":
                        return {"process_exit_code": exit_code, "timed_out": True, "cancelled": False,
                                "launch_error": False, "elapsed_ns": 1, "stdout_bytes": 0,
                                "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                                "stderr_sha256": r.sha256_bytes(b"")}
                    if request_hash == target["request_sha256"] and mode == "output_limit":
                        Path(result_path).write_bytes(b"x" * 8194)
                        return {"process_exit_code": exit_code, "timed_out": False, "cancelled": False,
                                "launch_error": False, "elapsed_ns": 1, "stdout_bytes": 0,
                                "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                                "stderr_sha256": r.sha256_bytes(b"")}
                    proc = real_invoke(binary, request_path, result_path, timeout)
                    if request_hash == target["request_sha256"] and mode in {"partial", "failed"}:
                        record, _, error = r._cpp_response(Path(result_path))
                        if error:
                            return proc
                        if mode == "partial":
                            record["status"] = "partial"
                            record["arms"]["matrix"] = {"arm_status": "not_run"}
                            record["errors"] = [{"scope": "matrix", "code": "controlled_partial", "path": "/arms/matrix"}]
                        else:
                            record["status"] = "failed"
                            record["arms"] = {"rvv": {"arm_status": "failed"}, "matrix": {"arm_status": "failed"}}
                            record["errors"] = [{"scope": "global", "code": "controlled_failure", "path": ""}]
                        Path(result_path).write_bytes(r.canonical_bytes(record) + b"\n")
                        proc["process_exit_code"] = exit_code
                    return proc

                with mock.patch.object(r, "invoke_evaluator", side_effect=controlled):
                    self.assertEqual(r.command_preflight(args), 3)
                verified = r.verify_bundle(root)
                self.assertEqual(verified["status"], "verified")
                ledger, _ = r.read_jsonl(root / "runner-ledger.jsonl")
                first = next(item for item in ledger if item["study_row_id"] == target["study_row_id"])
                self.assertEqual(first["runner_status"], expected_status)
                summary, _ = r.read_json(root / "summary.json")
                self.assertEqual(summary["study_status"], "pilot_failed")
                self.assertEqual(sum(row["runner_status"] == "not_started_due_to_preflight_failure" for row in summary["rows"]), 12)

    def test_v2_cpp_error_envelopes_and_eight_kib_result_cap_are_strict(self) -> None:
        r = self.runner
        row = self.rows_v2[0]
        invalid = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "invalid_request", "errors": [
                {"scope": "request", "code": "invalid_value", "path": "/workload/m"}
            ],
        }
        self.assertEqual(r._verify_cpp_error_envelope(invalid, "invalid_request", row)["code"], "invalid_value")
        internal = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "internal_error", "errors": [
                {"scope": "global", "code": "internal_error", "path": ""}
            ],
        }
        r._verify_cpp_error_envelope(internal, "internal_error", row)
        output_limit = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "output_limit", "candidate_pair_id": row["ids"]["candidate_pair_id"],
            "request_sha256": row["ids"]["request_sha256"],
            "model_assumptions_sha256": row["ids"]["model_assumptions_sha256"],
            "errors": [{"scope": "global", "code": "output_limit", "path": ""}],
        }
        r._verify_cpp_error_envelope(output_limit, "output_limit", row)
        bad = json.loads(json.dumps(output_limit))
        bad["model_assumptions_sha256"] = "0" * 64
        with self.assertRaises(r.RunnerError):
            r._verify_cpp_error_envelope(bad, "output_limit", row)
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-8k-cap-") as temp:
            path = Path(temp) / "result.json"
            path.write_bytes(b"{}" + b" " * (8192 - 2) + b"\n")
            parsed, raw, error = r._cpp_response(path, 8192)
            self.assertEqual(parsed, {})
            self.assertEqual(len(raw), 8193)
            self.assertIsNone(error)
            path.write_bytes(b"{}" + b" " * (8193 - 2) + b"\n")
            parsed, raw, error = r._cpp_response(path, 8192)
            self.assertIsNone(parsed)
            self.assertIsNone(raw)
            self.assertEqual(error, "result_output_limit")

    def test_v2_preflight_storage_and_result_caps_are_stricter_than_m63c(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63d-preflight-") as temp:
            root = Path(temp)
            (root / "manifest.json").write_bytes(r.canonical_bytes(self.manifest_v2) + b"\n")
            for row in self.rows_v2:
                (root / row["request_path"]).parent.mkdir(parents=True, exist_ok=True)
                (root / row["request_path"]).write_bytes(row["request_bytes"])
            request_bytes, final_bytes, peak_bytes = r._compute_storage_bound(root, self.rows_v2, 24)
            self.assertEqual(request_bytes, sum(len(row["request_bytes"]) for row in self.rows_v2))
            self.assertLess(peak_bytes, r.V2_LIMITS["max_bundle_bytes"])
            self.assertEqual(r.V2_LIMITS["max_result_payload_bytes"], 8192)
            self.assertEqual(final_bytes + r.MAX_STAGING, peak_bytes)

    def test_v2_plots_and_csv_are_deterministic_and_scientifically_labeled(self) -> None:
        r = self.runner
        synthetic = {}
        for row in self.rows_v2:
            synthetic[row["source_row_index"]] = {"runner_status": "failed", "error_codes": ["test_failed"], "artifact_paths": []}
        plots = r._make_v2_plots(self.rows_v2, synthetic)
        self.assertEqual(set(plots), set(r.V2_FILE_CAPS) & {"plots/compute-cycles.svg", "plots/logical-payload-bytes.svg", "plots/serialized-total-cycles.svg", "plots/serialized-cycle-breakdown.svg"})
        for data in plots.values():
            ET.fromstring(data)
            self.assertLessEqual(len(data), 65536)
        self.assertIn(b"modeled serial cycles", plots["plots/serialized-total-cycles.svg"])
        self.assertIn(b"modeled serial cycles", plots["plots/serialized-cycle-breakdown.svg"].lower())
        self.assertNotIn(b"measured speedup", plots["plots/serialized-total-cycles.svg"].lower())
        summary_rows = [r._summary_row_v2(row, {"runner_status": "not_started_due_to_limit", "error_codes": ["active_time_limit"]}) for row in self.rows_v2]
        csv_data = r._summary_csv(self.rows_v2, {}, summary_rows).encode("ascii")
        self.assertEqual(csv_data, r._summary_csv(self.rows_v2, {}, summary_rows).encode("ascii"))
        header = csv_data.splitlines()[0].decode("ascii").split(",")
        self.assertEqual(header, r.CSV_COLUMNS_V2)
        self.assertEqual(len(list(csv.DictReader(io.StringIO(csv_data.decode("ascii"))))), 24)

    def test_locked_manifest_expands_to_six_stable_requests(self) -> None:
        r = self.runner
        self.assertEqual(len(self.rows), 6)
        self.assertEqual([x["shape_id"] for x in self.rows], ["tiny", "tiny", "aligned", "aligned", "partial", "partial"])
        self.assertEqual([x["capacity_level_id"] for x in self.rows], ["p4", "p8"] * 3)
        self.assertEqual(len({x["request_bytes"] for x in self.rows}), 6)
        self.assertEqual(len({x["candidate_pair_id"] for x in self.rows}), 6)
        for row in self.rows:
            req = r.parse_json(row["request_bytes"].rstrip(b"\n"))
            self.assertEqual(r.sha256_bytes(r.canonical_bytes(req)), row["ids"]["request_sha256"])
            self.assertEqual(r.sha256_bytes(r.canonical_bytes(req["workload"])), row["ids"]["workload_id"])
        plan = r.make_plan(self.manifest, self.manifest_id, self.rows)
        self.assertEqual(plan["source_row_count"], 6)
        self.assertEqual(plan["unique_candidate_count"], 6)
        self.assertEqual([row["source_row_index"] for row in plan["rows"]], list(range(6)))

    def test_bundle_schema_versions_and_zero_indices_reject_boolean_aliases(self) -> None:
        r = self.runner
        self.assertTrue(r._has_schema_v1({"schema_id": "x", "schema_version": 1}, "x"))
        self.assertFalse(r._has_schema_v1({"schema_id": "x", "schema_version": True}, "x"))
        self.assertTrue(r._is_exact_index(0, 0))
        self.assertFalse(r._is_exact_index(False, 0))
        expected_plan = r.make_plan(self.manifest, self.manifest_id, self.rows)
        plan_raw = r.canonical_bytes(expected_plan) + b"\n"
        r._verify_plan_record(expected_plan, plan_raw, expected_plan)
        for mutate in (
            lambda value: value.__setitem__("schema_version", True),
            lambda value: value["rows"][0].__setitem__("source_row_index", False),
        ):
            changed = json.loads(json.dumps(expected_plan))
            mutate(changed)
            with self.assertRaises(r.RunnerError):
                r._verify_plan_record(changed, r.canonical_bytes(changed) + b"\n", expected_plan)
        phase = {
            "schema_id": "axissim.gemm-pair-study.host-diagnostic", "schema_version": True,
            "record_type": "phase", "phase": "preflight", "sequence": 0,
            "python_version": "3.12.0", "os_id": "darwin", "architecture_id": "arm64",
            "phase_elapsed_ns": 0,
        }
        with self.assertRaises(r.RunnerError):
            r._verify_host_diagnostics([phase])

    def test_manifest_rejects_schema_mutation_duplicate_keys_and_unsafe_values(self) -> None:
        r = self.runner
        changed = json.loads(json.dumps(self.manifest))
        changed["unexpected"] = True
        with self.assertRaises(r.RunnerError):
            r.validate_manifest(changed)
        changed = json.loads(json.dumps(self.manifest))
        changed["shapes"][0]["shape_id"] = "../escape"
        with self.assertRaises(r.RunnerError):
            r.validate_manifest(changed)
        with self.assertRaises(r.RunnerError):
            r.parse_json(b'{"a":1,"a":2}')
        with self.assertRaises(r.RunnerError):
            r.parse_json(b'{"a":1.0}')
        with self.assertRaises(r.RunnerError):
            r.parse_json(b'{"a":9007199254740992}')
        deep = b"[" * 34 + b"0" + b"]" * 34
        with self.assertRaises(r.RunnerError):
            r.parse_json(deep)
        with self.assertRaises(r.RunnerError):
            r.parse_json(b" " * 65537, max_bytes=65536)
        self.assertEqual(r.parse_json(b"{}" + b" " * (65536 - 2), max_bytes=65536), {})
        with self.assertRaises(r.RunnerError):
            r.parse_json(b"{}" + b" " * (65537 - 2), max_bytes=65536)

    def test_cpp_result_file_payload_cap_includes_final_lf(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-result-cap-") as temp:
            path = Path(temp) / "result.json"
            path.write_bytes(b"{}" + b" " * (65536 - 2) + b"\n")
            parsed, raw, error = r._cpp_response(path)
            self.assertEqual(parsed, {})
            self.assertIsNone(error)
            self.assertEqual(len(raw), 65537)
            path.write_bytes(b"{}" + b" " * (65537 - 2) + b"\n")
            parsed, raw, error = r._cpp_response(path)
            self.assertIsNone(parsed)
            self.assertIsNone(raw)
            self.assertEqual(error, "result_output_limit")

    def test_manifest_rejects_boolean_integer_aliases_and_nonboolean_flags(self) -> None:
        r = self.runner
        mutations = [
            ("workload", "batch_count", True),
            ("measurement", "repeat_count", True),
            ("mapping_constants.rvv", "lmul", True),
            ("mapping_constants.rvv", "chunk_setup_cycles", False),
            ("mapping_constants.matrix", "kernel_setup_cycles", False),
            ("workload", "transpose_a", 0),
            ("workload", "transpose_b", 1),
        ]
        for dotted_path, key, value in mutations:
            changed = json.loads(json.dumps(self.manifest))
            target = changed
            for part in dotted_path.split("."):
                target = target[part]
            target[key] = value
            with self.subTest(path=f"{dotted_path}.{key}"):
                with self.assertRaises(r.RunnerError):
                    r.validate_manifest(changed)

    def test_dedup_preserves_later_source_rows(self) -> None:
        r = self.runner
        manifest = json.loads(json.dumps(self.manifest))
        manifest["comparison_groups"].append({
            "group_id": "tiny_repeat", "shape_id": "tiny",
            "capacity_level_ids": ["p4", "p8"], "pilot_level_id": "p4",
        })
        rows = r.expand_candidates(manifest, r.canonical_hash(manifest))
        self.assertEqual(len(rows), 8)
        self.assertEqual(rows[6]["duplicate_of"], 0)
        self.assertEqual(rows[7]["duplicate_of"], 1)
        self.assertEqual(rows[6]["request_path"], rows[0]["request_path"])

    def test_path_and_artifact_caps_are_enforced_without_truncation(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-store-") as temp:
            root = Path(temp) / "bundle"
            root.mkdir()
            store = r.ArtifactStore(root)
            store.write("ok.txt", b"ok", cap=2)
            self.assertEqual((root / "ok.txt").read_bytes(), b"ok")
            with self.assertRaises(r.RunnerError) as over:
                store.write("large.txt", b"1234", cap=3)
            self.assertEqual(over.exception.code, "output_limit")
            with self.assertRaises(r.RunnerError) as traversal:
                store.write("../outside", b"x")
            self.assertEqual(traversal.exception.code, "unsafe_path")
            (root / "link").symlink_to(Path(temp))
            with self.assertRaises(r.RunnerError):
                store.write("link/escape", b"x")

    def test_artifact_atomic_write_cap_below_at_and_above_boundary(self) -> None:
        r = self.runner
        cap = 262144
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-exact-cap-") as temp:
            root = Path(temp) / "bundle"
            root.mkdir()
            store = r.ArtifactStore(root)
            store.write("summary-below.json", b"x" * (cap - 1), cap=cap)
            store.write("summary-at.json", b"x" * cap, cap=cap)
            self.assertEqual((root / "summary-at.json").stat().st_size, cap)
            with self.assertRaises(r.RunnerError) as too_large:
                store.write("summary-above.json", b"x" * (cap + 1), cap=cap)
            self.assertEqual(too_large.exception.code, "output_limit")

    def test_bundle_byte_cap_below_at_and_above_boundary(self) -> None:
        r = self.runner
        cap = r.FIXED_LIMITS["max_bundle_bytes"]
        for size, expected in ((cap - 1, True), (cap, True), (cap + 1, False)):
            with self.subTest(size=size), tempfile.TemporaryDirectory(prefix="axissim-m63c-bundle-cap-") as temp:
                root = Path(temp) / "bundle"
                root.mkdir()
                store = r.ArtifactStore(root)
                if expected:
                    store.write("bounded.bin", b"x" * size)
                    self.assertEqual(r.file_bytes_under(root), size)
                else:
                    with self.assertRaises(r.RunnerError) as too_large:
                        store.write("bounded.bin", b"x" * size)
                    self.assertEqual(too_large.exception.code, "output_limit")

    def test_pipe_drain_retains_only_count_and_hash(self) -> None:
        payload = b"x" * (2 * 1024 * 1024)
        digest = self.runner.PipeDigest()
        digest.consume(io.BytesIO(payload))
        self.assertEqual(digest.count, len(payload))
        self.assertEqual(digest.hexdigest(), hashlib.sha256(payload).hexdigest())
        self.assertFalse(hasattr(digest, "data"))

    def test_timeout_and_keyboard_cancellation_terminate_the_child(self) -> None:
        r = self.runner

        class Child:
            pid = 123456
            returncode = None

            def __init__(self, interrupt: bool = False) -> None:
                self.stdout = io.BytesIO(b"")
                self.stderr = io.BytesIO(b"")
                self.wait_calls = 0
                self.wait_timeouts: list[float | None] = []
                self.interrupt = interrupt
                self.terminated = False
                self.killed = False

            def wait(self, timeout=None):
                self.wait_calls += 1
                self.wait_timeouts.append(timeout)
                if self.wait_calls == 1:
                    if self.interrupt:
                        raise KeyboardInterrupt
                    raise subprocess.TimeoutExpired("fake", timeout)
                self.returncode = -15
                return self.returncode

            def terminate(self):
                self.terminated = True

            def kill(self):
                self.killed = True

        with tempfile.TemporaryDirectory(prefix="axissim-m63c-timeout-") as temp:
            request = Path(temp) / "request.json"
            result = Path(temp) / "result.json"
            request.write_bytes(b"{}\n")
            for interrupt in (False, True):
                child = Child(interrupt)
                killpg_context = (
                    mock.patch.object(r.os, "killpg")
                    if r.os.name == "posix"
                    else contextlib.nullcontext(mock.Mock())
                )
                with mock.patch.object(r.subprocess, "Popen", return_value=child), killpg_context as killpg:
                    proc = r.invoke_evaluator(self.binary, request, result, 0.01)
                self.assertEqual(proc["timed_out"], not interrupt)
                self.assertEqual(proc["cancelled"], interrupt)
                self.assertEqual(proc["process_exit_code"], 143)
                diagnostic = r._evaluator_diag("execute", 0, self.rows[0], proc)
                self.assertEqual(diagnostic["process_exit_code"], 143)
                if r.os.name == "posix":
                    killpg.assert_called_once()
                else:
                    self.assertTrue(child.terminated)

        child = Child()
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-zero-timeout-") as temp:
            request = Path(temp) / "request.json"
            request.write_bytes(b"{}\n")
            killpg_context = (
                mock.patch.object(r.os, "killpg")
                if r.os.name == "posix"
                else contextlib.nullcontext(mock.Mock())
            )
            with mock.patch.object(r.subprocess, "Popen", return_value=child), killpg_context:
                proc = r.invoke_evaluator(self.binary, request, Path(temp) / "result.json", 0)
        self.assertTrue(proc["timed_out"])
        self.assertEqual(child.wait_timeouts[0], 0)

        launch_proc = {"process_exit_code": None, "stdout_bytes": 0, "stdout_sha256": r.sha256_bytes(b""),
                       "stderr_bytes": 0, "stderr_sha256": r.sha256_bytes(b"")}
        launch_diag = r._evaluator_diag("execute", 1, self.rows[0], launch_proc)
        self.assertNotIn("process_exit_code", launch_diag)

    def test_preflight_rejects_over_budget_bundle_before_invoking_evaluator(self) -> None:
        r = self.runner
        manifest = json.loads(json.dumps(self.manifest))
        manifest["shapes"] = []
        manifest["comparison_groups"] = []
        for i in range(12):
            shape_id = f"shape{i:02d}"
            manifest["shapes"].append({
                "shape_id": shape_id, "m": i + 1, "n": 1, "k": 1,
                "tile_m": 1, "tile_n": 1, "tile_k": 1, "local_capacity_bytes": 32,
            })
            manifest["comparison_groups"].append({
                "group_id": f"group{i:02d}", "shape_id": shape_id,
                "capacity_level_ids": ["p4", "p8"], "pilot_level_id": "p4",
            })
        r.validate_manifest(manifest)
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-bcap-") as temp:
            temp_root = Path(temp)
            manifest_path = temp_root / "manifest.json"
            manifest_path.write_bytes(json.dumps(manifest).encode("ascii"))
            bundle = temp_root / "bundle"
            self.assertEqual(r.command_plan(argparse.Namespace(manifest=str(manifest_path), output_dir=str(bundle))), 0)
            with mock.patch.object(r, "invoke_evaluator", side_effect=AssertionError("preflight must refuse before child launch")):
                status = r.command_preflight(argparse.Namespace(manifest=str(manifest_path), binary=str(self.binary), output_dir=str(bundle)))
            self.assertEqual(status, 5)
            preflight, _ = r.read_json(bundle / "preflight.json")
            self.assertEqual(preflight["status"], "output_limit")
            self.assertGreater(preflight["reserved_peak_bytes"], r.FIXED_LIMITS["max_bundle_bytes"])
            summary, _ = r.read_json(bundle / "summary.json")
            ledger, _ = r.read_jsonl(bundle / "runner-ledger.jsonl")
            with (bundle / "summary.csv").open(newline="", encoding="ascii") as handle:
                csv_rows = list(r.csv.DictReader(handle))
            for summary_row, ledger_row, csv_row in zip(summary["rows"], ledger, csv_rows):
                self.assertEqual(summary_row["runner_status"], "not_started_due_to_limit")
                self.assertEqual(summary_row["error_codes"], ["output_limit"])
                self.assertEqual(ledger_row["error_codes"], ["output_limit"])
                self.assertEqual(csv_row["runner_status"], "not_started_due_to_limit")
                self.assertEqual(csv_row["error_codes"], "output_limit")
            r.verify_bundle(bundle)

    def test_reproduce_compares_valid_terminal_pilot_failure_bundles(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-failure-replay-") as temp:
            parent = Path(temp)
            reference = parent / "reference"
            replay = parent / "replay"
            def launch_failure(binary, root, row, timeout):
                return ({
                    "process_exit_code": None, "timed_out": False, "cancelled": False,
                    "launch_error": True, "elapsed_ns": 0, "stdout_bytes": 0,
                    "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                    "stderr_sha256": r.sha256_bytes(b""),
                }, None, None, None)

            with mock.patch.object(r, "_invoke_candidate", side_effect=launch_failure):
                self.assertEqual(r.command_plan(argparse.Namespace(manifest=str(self.manifest_path), output_dir=str(reference))), 0)
                preflight_code = r.command_preflight(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(reference)))
                self.assertEqual(preflight_code, 3)
                self.assertEqual(r.verify_bundle(reference)["status"], "verified")
                preflight, _ = r.read_json(reference / "preflight.json")
                self.assertEqual(preflight["status"], "pilot_failed")
                replay_code = r.command_reproduce(argparse.Namespace(
                    manifest=str(self.manifest_path), binary=str(self.binary),
                    reference_bundle=str(reference), output_dir=str(replay),
                ))
            self.assertEqual(replay_code, 0)
            self.assertEqual(r.verify_bundle(replay)["status"], "verified")

    def test_preflight_storage_bound_matches_independent_cap_sum(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-bound-formula-") as temp:
            bundle = Path(temp) / "bundle"
            self.assertEqual(r.command_plan(argparse.Namespace(manifest=str(self.manifest_path), output_dir=str(bundle))), 0)
            request_bytes, final_bound, peak_bound = r._compute_storage_bound(bundle, self.rows, len(self.rows))
            unique = len({row["request_bytes"] for row in self.rows})
            expected_requests = sum((bundle / row["request_path"]).stat().st_size for row in self.rows)
            self.assertEqual(request_bytes, expected_requests)
            expected_final = (
                (bundle / "manifest.json").stat().st_size
                + r.FILE_CAPS["plan.json"] + r.FILE_CAPS["preflight.json"] + r.FILE_CAPS["provenance.json"]
                + expected_requests + unique * (r.FIXED_LIMITS["max_result_payload_bytes"] + 1)
                + len(self.rows) * 2048 + r.FILE_CAPS["summary.json"] + r.FILE_CAPS["summary.csv"]
                + r.FILE_CAPS["report.md"] + r.FILE_CAPS["plots/compute-cycles.svg"]
                + r.FILE_CAPS["plots/logical-payload-bytes.svg"]
                + r.FILE_CAPS["canonical-artifact-manifest.json"]
                + (unique + 2) * 4096 + r.FILE_CAPS["bundle-index.json"]
            )
            self.assertEqual(final_bound, expected_final)
            self.assertEqual(peak_bound, expected_final + r.MAX_STAGING)

    def test_svg_is_deterministic_valid_xml_and_uses_only_modeled_terms(self) -> None:
        r = self.runner
        output: dict[int, dict[str, object]] = {}
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-svg-") as temp:
            root = Path(temp)
            binary_sha = r._file_hash(self.binary)
            for row in self.rows:
                request = root / f"{row['source_row_index']}.request.json"
                result_path = root / f"{row['source_row_index']}.result.json"
                request.write_bytes(row["request_bytes"])
                proc = r.invoke_evaluator(self.binary, request, result_path, 30)
                response, raw, parse_error = r._cpp_response(result_path)
                classified = r.classify_cpp_response(response, raw, parse_error, proc, row, binary_sha)
                self.assertEqual(classified["runner_status"], "complete")
                output[row["source_row_index"]] = classified
        plots_a = r._make_plots(self.rows, output)
        plots_b = r._make_plots(self.rows, output)
        self.assertEqual(plots_a, plots_b)
        for index, svg in enumerate(plots_a):
            root = ET.fromstring(svg)
            self.assertEqual(root.tag, "{http://www.w3.org/2000/svg}svg")
            text = svg.decode("utf-8").lower()
            self.assertNotIn("speedup", text)
            self.assertNotIn("power", text)
            if index == 0:
                self.assertNotIn("energy", text)
                self.assertIn("analytical compute cycles", text)
            else:
                self.assertIn("logical payload only", text)
                self.assertIn("not physical transfers", text)
                self.assertIn("or energy", text)
                self.assertNotIn("energy per", text)

    def test_real_evaluator_result_matches_the_planned_contract(self) -> None:
        r = self.runner
        row = self.rows[0]
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-cpp-") as temp:
            root = Path(temp)
            request = root / "request.json"
            result = root / "result.json"
            request.write_bytes(row["request_bytes"])
            proc = r.invoke_evaluator(self.binary, request, result, 30)
            self.assertEqual(proc["process_exit_code"], 0)
            response, raw, parse_error = r._cpp_response(result)
            self.assertIsNone(parse_error)
            valid, reason = r.validate_candidate_result(response, row, r._file_hash(self.binary))
            self.assertTrue(valid, reason)
            self.assertEqual(raw, r.canonical_bytes(response) + b"\n")

    def test_error_classification_never_creates_complete_pair_metrics(self) -> None:
        r = self.runner
        row = self.rows[0]
        proc = {"process_exit_code": 2, "timed_out": False, "launch_error": False}
        request_error = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "invalid_request", "errors": [{"scope": "request", "code": "invalid_value", "path": "/workload/m"}],
        }
        classified = r.classify_cpp_response(request_error, r.canonical_bytes(request_error) + b"\n", None, proc, row, "a" * 64)
        self.assertEqual(classified["runner_status"], "invalid")
        self.assertIsNone(classified["candidate_pair_id"])
        global_error = {"schema_id": "axissim.gemm-pair.error", "schema_version": 1, "status": "internal_error", "errors": [{"scope": "global", "code": "internal_error", "path": ""}]}
        failed = r.classify_cpp_response(global_error, r.canonical_bytes(global_error) + b"\n", None,
                                         {"process_exit_code": 4, "timed_out": False, "launch_error": False}, row, "a" * 64)
        self.assertEqual(failed["runner_status"], "failed")
        self.assertIsNone(failed["candidate_pair_id"])
        timeout = r.classify_cpp_response(None, None, None,
                                          {"process_exit_code": -15, "timed_out": True, "launch_error": False}, row, "a" * 64)
        self.assertEqual(timeout["runner_status"], "timeout")
        mismatch = r.classify_cpp_response(request_error, r.canonical_bytes(request_error) + b"\n", None,
                                           {"process_exit_code": 3, "timed_out": False, "launch_error": False}, row, "a" * 64)
        self.assertEqual(mismatch["runner_status"], "process_error")
        for exit_code in (0, 5):
            oversized = r.classify_cpp_response(None, None, "result_output_limit",
                                                {"process_exit_code": exit_code, "timed_out": False, "launch_error": False}, row, "a" * 64)
            self.assertEqual(oversized["runner_status"], "output_limit")

        overlong_path = {
            "status": "invalid_request",
            "errors": [{"code": "invalid_value", "path": "/" + "x" * 300}],
        }
        self.assertEqual(r._error_codes(overlong_path), ["error_path_limit"])
        self.assertEqual(r._error_paths(overlong_path), [""])

    def test_terminal_ledger_mappings_are_strict_and_reject_corruption(self) -> None:
        r = self.runner
        row = self.rows[0]
        result = row["result_path"]
        request_error = f"requests/{row['study_row_id']}.request.json"
        error_result = f"results/{row['study_row_id']}.error.json"
        pair = row["candidate_pair_id"]
        valid_records = [
            r._ledger_record(row, "duplicate", candidate_pair_id=pair),
            r._ledger_record(row, "not_started_due_to_limit", candidate_pair_id=pair, error_codes=["active_time_limit"]),
            r._ledger_record(row, "not_started_provenance_failure", candidate_pair_id=pair, error_codes=["provenance_failed"]),
            r._ledger_record(row, "not_started_due_to_preflight_failure", candidate_pair_id=pair, error_codes=["pilot_failed"]),
            r._ledger_record(row, "timeout", execution_order=0, process_exit=143, error_codes=["evaluator_timeout"]),
            r._ledger_record(row, "missing_result", execution_order=0, process_exit=0, error_codes=["missing_result"]),
            r._ledger_record(row, "process_error", execution_order=0, error_codes=["process_launch_failed"]),
            r._ledger_record(row, "invalid", execution_order=0, process_exit=2, cpp_status="invalid_request", error_codes=["invalid_value"], artifacts=[request_error, error_result]),
            r._ledger_record(row, "partial", execution_order=0, process_exit=3, cpp_status="partial", rvv="complete", matrix="invalid", candidate_pair_id=pair, error_codes=["capacity_exceeded"], artifacts=[result]),
            r._ledger_record(row, "failed", execution_order=0, process_exit=4, cpp_status="failed", rvv="failed", matrix="failed", candidate_pair_id=pair, error_codes=["mapping_failed"], artifacts=[result]),
            r._ledger_record(row, "failed", execution_order=0, process_exit=4, cpp_status="internal_error", error_codes=["internal_error"], artifacts=[error_result]),
            r._ledger_record(row, "identity_mismatch", execution_order=0, process_exit=0, cpp_status="complete", rvv="complete", matrix="complete", candidate_pair_id=pair, error_codes=["identity_mismatch"], artifacts=[result]),
            r._ledger_record(row, "result_schema_error", execution_order=0, process_exit=0, candidate_pair_id=pair, error_codes=["result_schema_error"], artifacts=[result]),
            r._ledger_record(row, "output_limit", execution_order=0, process_exit=5, cpp_status="output_limit", candidate_pair_id=pair, error_codes=["output_limit"], artifacts=[result]),
            r._ledger_record(row, "output_limit", execution_order=0, process_exit=0, error_codes=["result_output_limit"]),
            r._ledger_record(row, "output_limit", execution_order=0, process_exit=5, error_codes=["result_output_limit"]),
        ]
        for record in valid_records:
            with self.subTest(status=record["runner_status"]):
                r._verify_terminal_row_fields(record["runner_status"], record, row)
                damaged = dict(record)
                damaged["artifact_paths"] = ["results/unrelated.error.json"]
                with self.assertRaises(r.RunnerError):
                    r._verify_terminal_row_fields(record["runner_status"], damaged, row)
        for invalid_code in (True, -1, 1.5, "0", None):
            with self.subTest(process_exit_code=invalid_code):
                damaged = dict(valid_records[-1])
                damaged["process_exit_code"] = invalid_code
                with self.assertRaises(r.RunnerError):
                    r._verify_terminal_row_fields(damaged["runner_status"], damaged, row)
        runner_output_limit = valid_records[-1]
        for field, value in (("candidate_pair_id", pair), ("error_codes", ["result_output_limit", "fake"]),
                             ("error_paths", ["/fabricated"]), ("matrix_arm_status", "complete")):
            with self.subTest(runner_output_limit_field=field):
                damaged = dict(runner_output_limit)
                damaged[field] = value
                with self.assertRaises(r.RunnerError):
                    r._verify_terminal_row_fields(damaged["runner_status"], damaged, row)

    def test_ledger_launch_status_must_match_host_diagnostic(self) -> None:
        r = self.runner
        row = self.rows[0]
        phase = {
            "schema_id": "axissim.gemm-pair-study.host-diagnostic", "schema_version": 1,
            "record_type": "phase", "phase": "preflight", "sequence": 0,
            "python_version": "3.12.0", "os_id": "darwin", "architecture_id": "arm64",
            "phase_elapsed_ns": 1,
        }
        evaluator = {
            "schema_id": "axissim.gemm-pair-study.host-diagnostic", "schema_version": 1,
            "record_type": "evaluator", "phase": "preflight", "sequence": 0,
            "python_version": "3.12.0", "os_id": "darwin", "architecture_id": "arm64",
            "study_row_id": row["study_row_id"], "evaluator_elapsed_ns": 1,
            "stdout_bytes": 0, "stdout_sha256": r.sha256_bytes(b""),
            "stderr_bytes": 0, "stderr_sha256": r.sha256_bytes(b""), "process_exit_code": 0,
        }
        diagnostics = r._verify_host_diagnostics([phase, evaluator])
        ledger = r._ledger_record(row, "complete", execution_order=0, process_exit=0)
        r._verify_ledger_host_launch(row["study_row_id"], ledger, diagnostics)
        for bad_code in (-1, True, 1.5):
            with self.subTest(host_code=bad_code):
                changed = dict(evaluator, process_exit_code=bad_code)
                with self.assertRaises(r.RunnerError):
                    r._verify_host_diagnostics([phase, changed])
        with self.assertRaises(r.RunnerError):
            r._verify_ledger_host_launch(row["study_row_id"], dict(ledger, process_exit_code=7), diagnostics)
        with self.assertRaises(r.RunnerError):
            r._verify_ledger_host_launch(row["study_row_id"], dict(ledger, execution_order=1), diagnostics)
        with self.assertRaises(r.RunnerError):
            r._verify_ledger_host_launch(row["study_row_id"], {k: v for k, v in ledger.items() if k != "execution_order"}, diagnostics)

    def test_orphan_host_evaluator_diagnostics_are_rejected(self) -> None:
        r = self.runner
        row = self.rows[0]
        ledger = [r._ledger_record(row, "complete", execution_order=0, process_exit=0)]
        diagnostics = {row["study_row_id"]: {"sequence": 0}}
        r._verify_diagnostic_ledger_set(ledger, diagnostics)
        with self.assertRaises(r.RunnerError):
            r._verify_diagnostic_ledger_set(ledger, {**diagnostics, "b" * 64: {"sequence": 1}})
        with self.assertRaises(r.RunnerError):
            r._verify_diagnostic_ledger_set([r._ledger_record(row, "duplicate")], diagnostics)

    def test_cpp_error_envelopes_are_strict_and_request_bound(self) -> None:
        r = self.runner
        row = self.rows[0]
        invalid = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "invalid_request", "errors": [
                {"scope": "request", "code": "invalid_value", "path": "/workload/m"}
            ],
        }
        item = r._verify_cpp_error_envelope(invalid, "invalid_request", row)
        self.assertEqual(item["code"], "invalid_value")
        internal = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "internal_error", "errors": [
                {"scope": "global", "code": "internal_error", "path": ""}
            ],
        }
        r._verify_cpp_error_envelope(internal, "internal_error", row)
        output_limit = {
            "schema_id": "axissim.gemm-pair.error", "schema_version": 1,
            "status": "output_limit", "candidate_pair_id": row["ids"]["candidate_pair_id"],
            "request_sha256": row["ids"]["request_sha256"],
            "model_assumptions_sha256": row["ids"]["model_assumptions_sha256"],
            "errors": [{"scope": "global", "code": "output_limit", "path": ""}],
        }
        r._verify_cpp_error_envelope(output_limit, "output_limit", row)
        mutations = [
            lambda x: x.__setitem__("schema_version", True),
            lambda x: x["errors"][0].__setitem__("scope", "global"),
            lambda x: x["errors"][0].__setitem__("path", "bad-pointer"),
        ]
        for mutate in mutations:
            changed = json.loads(json.dumps(invalid))
            mutate(changed)
            with self.assertRaises(r.RunnerError):
                r._verify_cpp_error_envelope(changed, "invalid_request", row)
        changed = json.loads(json.dumps(output_limit))
        changed["request_sha256"] = "0" * 64
        with self.assertRaises(r.RunnerError):
            r._verify_cpp_error_envelope(changed, "output_limit", row)

    def test_malformed_nested_cpp_result_is_not_eligible_as_complete(self) -> None:
        r = self.runner
        row = self.rows[0]
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-bad-result-") as temp:
            root = Path(temp)
            request = root / "request.json"
            result = root / "result.json"
            request.write_bytes(row["request_bytes"])
            proc = r.invoke_evaluator(self.binary, request, result, 30)
            response, _, error = r._cpp_response(result)
            self.assertIsNone(error)
            response["arms"]["rvv"]["logical_traffic"] = {}
            raw = r.canonical_bytes(response) + b"\n"
            valid, reason = r.validate_candidate_result(response, row, r._file_hash(self.binary))
            self.assertFalse(valid)
            self.assertEqual(reason, "result_schema_error")
            classified = r.classify_cpp_response(response, raw, None, proc, row, r._file_hash(self.binary))
            self.assertEqual(classified["runner_status"], "result_schema_error")
            self.assertEqual(classified["error_codes"], ["result_schema_error"])

    def test_cpp_result_enforces_schema_coverage_and_paired_semantics(self) -> None:
        r = self.runner
        row = self.rows[0]
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-cross-field-") as temp:
            root = Path(temp)
            request = root / "request.json"
            result = root / "result.json"
            request.write_bytes(row["request_bytes"])
            proc = r.invoke_evaluator(self.binary, request, result, 30)
            self.assertEqual(proc["process_exit_code"], 0)
            base, _, error = r._cpp_response(result)
            self.assertIsNone(error)
            mutations = [
                ("boolean schema version", lambda x: x.__setitem__("schema_version", True)),
                ("extra consumed input", lambda x: x["simulator"]["consumed_inputs"].append("unused.json")),
                ("contradictory coverage", lambda x: x["model_coverage"].__setitem__("energy", "modeled")),
                ("matrix capacity", lambda x: x["arms"]["matrix"].__setitem__("capacity_fit", False)),
                ("different workload count", lambda x: x["arms"]["matrix"]["workload_counts"].__setitem__("useful_macs_per_unit", 1)),
                ("inconsistent p99 rank", lambda x: x["arms"]["rvv"]["p99_compute_completion"].__setitem__("p99_rank", 2)),
                ("inconsistent p99 numerator", lambda x: x["arms"]["matrix"]["p99_compute_completion"].__setitem__("numerator_cycles", 0)),
            ]
            for label, mutate in mutations:
                with self.subTest(case=label):
                    changed = json.loads(json.dumps(base))
                    mutate(changed)
                    valid, reason = r.validate_candidate_result(changed, row, r._file_hash(self.binary))
                    self.assertFalse(valid)
                    self.assertEqual(reason, "result_schema_error")

    def test_hypothesis_decisions_and_report_define_claims_and_candidate_crosswalk(self) -> None:
        r = self.runner
        rows_by_key = {(row["comparison_group_id"], row["capacity_level_id"]): row for row in self.rows}
        equal_results: dict[int, dict[str, object]] = {}
        for row in self.rows:
            traffic = {"read_a_bytes_per_unit": 10, "read_b_bytes_per_unit": 20, "write_c_bytes_per_unit": 30}
            arms = {name: {"compute_cycles_per_unit": 100, "logical_traffic": dict(traffic)} for name in ("rvv", "matrix")}
            equal_results[row["source_row_index"]] = {"runner_status": "complete", "record": {"arms": arms}}
        h1, h2 = r._hypothesis_results(self.manifest, self.rows, equal_results)
        self.assertEqual(h1["status"], "contradicted")
        self.assertEqual(h2["status"], "contradicted")

        partial = dict(equal_results)
        partial[self.rows[0]["source_row_index"]] = {"runner_status": "partial"}
        h1, h2 = r._hypothesis_results(self.manifest, self.rows, partial)
        self.assertEqual(h1["status"], "inconclusive")
        self.assertEqual(h2["status"], "inconclusive")

        changed = dict(equal_results)
        group_id = self.manifest["comparison_groups"][0]["group_id"]
        p4 = rows_by_key[(group_id, "p4")]
        p8 = rows_by_key[(group_id, "p8")]
        changed[p8["source_row_index"]] = {"runner_status": "complete", "record": {"arms": {
            "rvv": {"compute_cycles_per_unit": 90, "logical_traffic": {"read_a_bytes_per_unit": 10, "read_b_bytes_per_unit": 20, "write_c_bytes_per_unit": 30}},
            "matrix": {"compute_cycles_per_unit": 100, "logical_traffic": {"read_a_bytes_per_unit": 11, "read_b_bytes_per_unit": 20, "write_c_bytes_per_unit": 30}},
        }}}
        h1, h2 = r._hypothesis_results(self.manifest, self.rows, changed)
        self.assertEqual(h1["status"], "supported")
        self.assertEqual(h2["status"], "supported")
        report = r._make_report(self.manifest, self.rows, changed, "complete")
        self.assertIn("Preregistered statement", report)
        self.assertIn(r.HYPOTHESES[0]["statement"], report)
        self.assertIn(r.HYPOTHESES[1]["statement"], report)
        self.assertIn("Decision rule: supported if any complete", report)
        self.assertIn(p4["candidate_pair_id"], report)
        self.assertIn(p8["candidate_pair_id"], report)
        self.assertIn("RVV VLEN/SEW/LMUL", report)
        self.assertIn("Matrix local capacity bytes", report)
        self.assertIn("matrix slots per cycle", report)

    def _prepare(self, root: Path) -> None:
        r = self.runner
        self.assertEqual(r.command_plan(argparse.Namespace(manifest=str(self.manifest_path), output_dir=str(root))), 0)
        self.assertEqual(r.command_preflight(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root))), 0)

    def _set_preflight_elapsed(self, root: Path, elapsed_ns: int) -> None:
        r = self.runner
        records, _ = r.read_jsonl(root / "host-diagnostics.jsonl")
        phase_rows = [row for row in records if row.get("record_type") == "phase" and row.get("phase") == "preflight"]
        self.assertEqual(len(phase_rows), 1)
        phase_rows[0]["phase_elapsed_ns"] = elapsed_ns
        diagnostic_bytes = r._jsonl_bytes(records)
        (root / "host-diagnostics.jsonl").write_bytes(diagnostic_bytes)

    def test_cumulative_active_time_deadline_just_below_at_and_above(self) -> None:
        r = self.runner
        cases = ((899_999_999_999, 1, "timeout"), (900_000_000_000, 0, None), (900_000_000_001, 0, None))
        for elapsed_ns, expected_launches, expected_state in cases:
            with self.subTest(elapsed_ns=elapsed_ns), tempfile.TemporaryDirectory(prefix="axissim-m63c-deadline-") as temp:
                root = Path(temp) / "bundle"
                self._prepare(root)
                self._set_preflight_elapsed(root, elapsed_ns)
                proc = {
                    "process_exit_code": 143, "timed_out": True, "cancelled": False,
                    "launch_error": False, "elapsed_ns": 1, "stdout_bytes": 0,
                    "stdout_sha256": r.sha256_bytes(b""), "stderr_bytes": 0,
                    "stderr_sha256": r.sha256_bytes(b""),
                }
                with mock.patch.object(r.time, "monotonic_ns", return_value=123456), \
                     mock.patch.object(r, "_invoke_candidate", return_value=(proc, None, None, None)) as invoke, \
                     mock.patch("sys.stdout", new=io.StringIO()):
                    result_code = r.command_execute(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root)))
                self.assertEqual(result_code, 3)
                self.assertEqual(invoke.call_count, expected_launches)
                verification = r.verify_bundle(root)
                self.assertEqual(verification["status"], "verified")
                summary, _ = r.read_json(root / "summary.json")
                self.assertEqual(summary["study_status"], "limit_reached")
                states = [row["runner_status"] for row in summary["rows"]]
                if expected_state:
                    self.assertIn(expected_state, states)
                else:
                    self.assertNotIn("timeout", states)
                self.assertEqual(states.count("not_started_due_to_limit"), 3 - expected_launches)

    def test_partial_result_is_retained_but_excluded_from_pair_reports(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-partial-") as temp:
            root = Path(temp) / "bundle"
            self._prepare(root)
            real_invoke = r.invoke_evaluator

            def controlled(binary, request_path, result_path, timeout):
                proc = real_invoke(binary, request_path, result_path, timeout)
                request = r.parse_json(request_path.read_bytes().rstrip(b"\n"))
                if request["mappings"]["rvv"]["lanes"] == 8 and request["workload"]["m"] == 2:
                    response, _, error = r._cpp_response(result_path)
                    if error:
                        return proc
                    response["status"] = "partial"
                    response["arms"]["matrix"] = {"arm_status": "invalid"}
                    response["errors"] = [{"scope": "matrix", "code": "capacity_exceeded", "path": "/mappings/matrix/local_capacity_bytes"}]
                    result_path.write_bytes(r.canonical_bytes(response) + b"\n")
                    proc["process_exit_code"] = 3
                return proc

            with mock.patch.object(r, "invoke_evaluator", side_effect=controlled):
                self.assertEqual(r.command_execute(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root))), 3)
            verification = r.verify_bundle(root)
            self.assertEqual(verification["status"], "verified")
            summary, _ = r.read_json(root / "summary.json")
            partial = next(row for row in summary["rows"] if row["shape_id"] == "tiny" and row["capacity_level_id"] == "p8")
            self.assertEqual(partial["runner_status"], "partial")
            self.assertNotIn("metrics", partial)
            plot, _ = r._make_plots(self.rows, {
                row["source_row_index"]: {"runner_status": "partial", "record": {}}
                for row in self.rows if row["shape_id"] == "tiny" and row["capacity_level_id"] == "p8"
            })
            self.assertNotIn(b"tiny / P8", plot)

    def test_malformed_complete_result_is_preserved_but_excluded_and_verifiable(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-malformed-bundle-") as temp:
            root = Path(temp) / "bundle"
            self._prepare(root)
            real_invoke = r.invoke_evaluator

            def controlled(binary, request_path, result_path, timeout):
                proc = real_invoke(binary, request_path, result_path, timeout)
                request = r.parse_json(request_path.read_bytes().rstrip(b"\n"))
                if request["workload"]["m"] == 2 and request["mappings"]["matrix"]["mac_slots_per_cycle"] == 8:
                    response, _, error = r._cpp_response(result_path)
                    if error:
                        return proc
                    response["arms"]["rvv"]["logical_traffic"] = {}
                    result_path.write_bytes(r.canonical_bytes(response) + b"\n")
                return proc

            with mock.patch.object(r, "invoke_evaluator", side_effect=controlled):
                self.assertEqual(r.command_execute(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root))), 3)
            self.assertEqual(r.verify_bundle(root)["status"], "verified")
            summary, _ = r.read_json(root / "summary.json")
            invalid = next(row for row in summary["rows"] if row["shape_id"] == "tiny" and row["capacity_level_id"] == "p8")
            self.assertEqual(invalid["runner_status"], "result_schema_error")
            self.assertNotIn("metrics", invalid)
            self.assertEqual(invalid["error_codes"], ["result_schema_error"])

    def test_process_failure_is_isolated_and_later_candidates_continue(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-failure-") as temp:
            root = Path(temp) / "bundle"
            self._prepare(root)
            real_invoke = r.invoke_evaluator
            injected = False

            def controlled(binary, request_path, result_path, timeout):
                nonlocal injected
                request = r.parse_json(request_path.read_bytes().rstrip(b"\n"))
                if not injected and request["mappings"]["rvv"]["lanes"] == 8 and request["workload"]["m"] == 2:
                    injected = True
                    return {"process_exit_code": 1, "timed_out": False, "cancelled": False, "launch_error": False,
                            "elapsed_ns": 100, "stdout_bytes": 0, "stdout_sha256": r.sha256_bytes(b""),
                            "stderr_bytes": 0, "stderr_sha256": r.sha256_bytes(b"")}
                return real_invoke(binary, request_path, result_path, timeout)

            with mock.patch.object(r, "invoke_evaluator", side_effect=controlled):
                self.assertEqual(r.command_execute(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root))), 3)
            r.verify_bundle(root)
            ledger, _ = r.read_jsonl(root / "runner-ledger.jsonl")
            self.assertEqual(ledger[1]["runner_status"], "missing_result")
            self.assertTrue(all(row["runner_status"] == "complete" for row in ledger[2:]))

    def test_bundle_verifier_rejects_tampered_summary(self) -> None:
        r = self.runner
        with tempfile.TemporaryDirectory(prefix="axissim-m63c-tamper-") as temp:
            root = Path(temp) / "bundle"
            self._prepare(root)
            self.assertEqual(r.command_execute(argparse.Namespace(manifest=str(self.manifest_path), binary=str(self.binary), output_dir=str(root))), 0)
            summary_path = root / "summary.json"
            summary = json.loads(summary_path.read_text())
            summary["rows"][0]["metrics"]["rvv"]["compute_cycles_per_unit"] += 1
            summary_path.write_bytes(r.canonical_bytes(summary) + b"\n")
            with self.assertRaises(r.RunnerError):
                r.verify_bundle(root)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
