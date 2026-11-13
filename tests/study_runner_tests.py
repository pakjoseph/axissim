#!/usr/bin/env python3
"""Unit and process-contract tests for M4 plan/preflight orchestration."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal


def load_runner(source_root: pathlib.Path):
    sys.path.insert(0, str(source_root / "experiments"))
    import run_budget_constrained_study as runner  # pylint: disable=import-outside-toplevel
    return runner


class StudyRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        parser = argparse.ArgumentParser()
        parser.add_argument("--source-root", required=True, type=pathlib.Path)
        parser.add_argument("--binary", required=True, type=pathlib.Path)
        args, _ = parser.parse_known_args()
        cls.root = args.source_root.resolve()
        cls.binary = args.binary.resolve()
        cls.runner = load_runner(cls.root)
        cls.final_manifest_path = cls.root / "experiments/budget_constrained_study/manifest.json"
        cls.final_manifest = cls.runner.load_json(cls.final_manifest_path)

    def setUp(self) -> None:
        self.runner = self.__class__.runner
        self.root = self.__class__.root
        self.binary = self.__class__.binary
        self.build_dir = self.root / "build"
        self.build_dir.mkdir(exist_ok=True)

    def _write_fixture_paths(self, manifest: dict) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
        manifest = copy.deepcopy(manifest)
        plan_path = self.build_dir / "study-runner-test-plan.json"
        report_path = self.build_dir / "study-runner-test-report.json"
        manifest_path = self.build_dir / "study-runner-test-manifest.json"
        manifest["planning_report_path"] = plan_path.relative_to(self.root).as_posix()
        build_id = self.runner.file_sha256(self.binary)
        source_hashes = self.runner.calculate_source_hashes(self.root, self.runner.source_paths(manifest))
        pilots = []
        pilot_values = {
            "physical_ai_baseline": (240, 12991, 2815, 0.01),
            "mnist12_profiled": (720, 43130, 5103, 0.02),
            "gemm_heavy_512": (240, 13022, 2818, 0.01),
        }
        for workload_id, (records, trace_bytes, result_bytes, wall_seconds) in pilot_values.items():
            pilots.append({"workload_id": workload_id, "legacy_trace_record_count": records,
                           "legacy_trace_bytes": trace_bytes, "legacy_result_bytes": result_bytes,
                           "pilot_wall_seconds": wall_seconds})
        plan = {"schema_id": "axissim.study-plan-report.v1", "plan_id": "a" * 64,
                "simulator_build_identity": build_id, "source_hashes": source_hashes,
                "candidate_count_unique": 372,
                "host_diagnostics": {"pilots": pilots}}
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        manifest_path.write_bytes(self.runner.canonical_json(manifest) + b"\n")
        return manifest_path, plan_path, report_path

    def test_canonicalization_vectors(self) -> None:
        value = json.loads('{"b":1.0,"a":"e\\u0301"}', parse_float=Decimal, parse_int=Decimal)
        canonical = self.runner.canonical_json(value)
        self.assertEqual(canonical, '{"a":"é","b":1}'.encode())
        self.assertEqual(hashlib.sha256(canonical).hexdigest(),
                         "aa58fba8483623bed37c1b02edfccbdd9a53123837c20bfa4cb4049993a2872e")
        control = json.loads('{"s":"a\\nb\\u0000"}', parse_float=Decimal, parse_int=Decimal)
        control_bytes = self.runner.canonical_json(control)
        self.assertEqual(hashlib.sha256(control_bytes).hexdigest(),
                         "d56c1daf0cc91d68e6df3a1e1ad1e66d42a40f05c9276c6cc5e635803d6903fd")
        self.assertEqual(self.runner.canonical_json(Decimal("-0.0")), b"0")
        candidate = {
            "configuration_identity": "0" * 64,
            "factor_assignments": [{"factor_id": "rvv_lanes", "value": 8}],
            "manifest_id": "1" * 64,
            "measurement_protocol_identity": "5" * 64,
            "model_assumption_identity": "4" * 64,
            "simulator_build_identity": "2" * 64,
            "workload_identity": "3" * 64,
        }
        self.assertEqual(self.runner.digest(candidate),
                         "a0bc2e8d26e56b33edd19d54019a0ec8dc1d98a4ae11d6503494d9b5498d1b8c")
        coverage = [{"factor_id": "dram_bandwidth_gbps",
                     "omitted_terms": ["missing.static.dram_interface_w_per_gbps"],
                     "power_coverage": "partial",
                     "represented_terms": ["static.dram_interface_w"]}]
        self.assertEqual(self.runner.digest(coverage),
                         "6768b0b1da5098bad1a8460e36cc9d8960f535a5b82340be58fb93fd98b0ad7f")
        with self.assertRaises(self.runner.StudyError):
            self.runner.canonical_json({"é": 1, "e\u0301": 2})
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as duplicate_json:
            duplicate_json.write('{"same":1,"same":2}')
            duplicate_json.flush()
            with self.assertRaises(self.runner.StudyError):
                self.runner.load_json(pathlib.Path(duplicate_json.name))

    def test_manifest_candidate_enumeration_is_exact_and_deterministic(self) -> None:
        manifest = self.final_manifest
        self.runner._validate_top_level(manifest, final=True)
        hashes = self.runner.calculate_source_hashes(self.root, self.runner.source_paths(manifest))
        build_id = self.runner.file_sha256(self.binary)
        first, n_a = self.runner.enumerate_candidates(manifest, build_id, hashes)
        second, n_b = self.runner.enumerate_candidates(manifest, build_id, hashes)
        self.assertEqual([c["candidate_id"] for c in first], [c["candidate_id"] for c in second])
        self.assertEqual(n_a, n_b)
        self.assertEqual(len(first), 168)
        group_order = {group["group_id"]: index for index, group in enumerate(manifest["groups"])}
        workload_order = {workload["workload_id"]: index for index, workload in enumerate(manifest["workloads"])}
        workload_by_id = {workload["workload_id"]: workload for workload in manifest["workloads"]}
        stratum_order = {
            workload_id: {stratum["release_stratum_id"]: index
                          for index, stratum in enumerate(workload["release_strata"])}
            for workload_id, workload in workload_by_id.items()
        }
        traversal_keys = [
            (group_order[candidate["group_memberships"][0]["group_id"]],
             workload_order[candidate["workload_id"]],
             stratum_order[candidate["workload_id"]][candidate["release_stratum_id"]],
             candidate["group_memberships"][0]["row_index"])
            for candidate in first
        ]
        self.assertEqual(traversal_keys, sorted(traversal_keys))
        self.assertEqual({c["family_id"] for c in first}, {"physical_ai", "mnist12", "gemm_heavy"})
        self.assertEqual({c["release_stratum_id"] for c in first}, {"periodic_30fps", "trace_30fps"})
        first_interaction = next(index for index, group in enumerate(manifest["groups"])
                                 if group["group_id"].startswith("interaction:"))
        self.assertTrue(all(group["group_id"].startswith("oat:") for group in manifest["groups"][:first_interaction]))
        self.assertTrue(all(group["group_id"].startswith("interaction:") for group in manifest["groups"][first_interaction:]))
        self.assertEqual(
            {group["group_id"] for group in manifest["groups"] if group["group_id"].startswith("interaction:")},
            {"interaction:npu_macs_x_dram_bandwidth", "interaction:sram_capacity_x_tile_mapping",
             "interaction:scheduler_policy_x_rvv_chunk"},
        )
        tile_group = next(group for group in manifest["groups"] if group["group_id"] == "interaction:sram_capacity_x_tile_mapping")
        self.assertEqual(len(tile_group["candidate_rows"]), 5)
        self.assertEqual(len({self.runner.canonical_json(row) for row in tile_group["candidate_rows"]}), 5)
        mnist = next(w for w in manifest["workloads"] if w["family_id"] == "mnist12")
        self.assertEqual(mnist["applicability"]["npu_tile_m"], "n/a")
        self.assertEqual(mnist["applicability"]["rvv_chunk_elements"], "n/a")

    def test_full_plan_reports_unfit_rows_without_pruning_them(self) -> None:
        report_path = self.build_dir / "study-runner-full-plan-test.json"
        report = self.runner.plan(
            self.root,
            self.root / "experiments/budget_constrained_study/planning_manifest.json",
            self.binary,
            report_path,
        )
        self.assertEqual(report["status"], "planned_with_invalid_rows")
        self.assertEqual(report["candidate_count_unique"], 372)
        self.assertEqual(report["candidate_count_before_deduplication"], 398)
        self.assertEqual(len(report["candidates"]), 372)
        invalid = [candidate for candidate in report["candidates"]
                   if candidate["configuration_validation_status"] != "valid"]
        self.assertEqual(len(invalid), 18)
        invalid_group_ids = {member["group_id"] for candidate in invalid for member in candidate["group_memberships"]}
        self.assertEqual(invalid_group_ids, {"interaction:sram_capacity_x_tile_mapping", "oat:weight_sram_capacity_bytes"})
        pilots = report["host_diagnostics"]["pilots"]
        self.assertEqual({pilot["workload_id"] for pilot in pilots},
                         {"physical_ai_baseline", "mnist12_profiled", "gemm_heavy_512"})
        self.assertTrue(all(pilot["pilot_process_exit_status"] == 0 and
                            pilot["legacy_result_schema_version"] == "1.2" and
                            pilot["pilot_structured_result_status"] == "available_schema_1.2" for pilot in pilots))
        projection = report["host_diagnostics"]["planning_projection"]
        self.assertGreater(projection["estimated_total_storage_bytes_excluding_unfinalized_candidate_requests"], 0)
        self.assertFalse(projection["candidate_request_artifacts_included"])
        self.assertGreater(projection["host_wall_seconds_including_one_worst_case_trace_replay_per_candidate"], 0)
        scheduler_groups = [group for group in report["group_summary"]
                            if group["group_id"] == "oat:scheduler_policy"]
        self.assertEqual({group["release_stratum_id"] for group in scheduler_groups},
                         {"periodic_30fps", "trace_30fps"})
        self.assertTrue(all(group["cross_workload_or_release_comparison"] is False for group in scheduler_groups))

    def test_final_manifest_keeps_the_exact_study_only_mnist_overlay(self) -> None:
        workload = next(w for w in self.final_manifest["workloads"] if w["family_id"] == "mnist12")
        fields = workload["power_overlay"]["fields"]
        self.assertEqual(set(fields), set(self.runner.MNIST_OVERLAY_FIELDS))
        self.assertEqual(fields["it_power_budget_w"], Decimal("0.23"))
        self.assertEqual(fields["static_power_w"], Decimal("0.05"))
        self.assertEqual(fields["npu_static_power_per_mac_w"], Decimal("0.00003"))
        self.assertEqual(fields["rvv_static_power_per_lane_w"], Decimal("0.002"))
        self.assertEqual(fields["sram_static_power_per_kib_w"], Decimal("0.0001"))
        self.assertEqual(fields["dram_interface_static_power_w"], Decimal("0.02"))
        self.assertEqual(self.final_manifest["power_budget_w"], Decimal("0.23"))

    def test_over_cap_rejection_precedes_any_candidate_validation(self) -> None:
        manifest = copy.deepcopy(self.final_manifest)
        manifest["limits"]["max_candidates"] = 167
        manifest_path, plan_path, report_path = self._write_fixture_paths(manifest)
        invocations: list[list[str]] = []

        def validator(command: list[str], _cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
            invocations.append(command)
            return subprocess.CompletedProcess(command, 0, "Config is valid: test\n", "")

        result = self.runner.preflight(self.root, manifest_path, self.binary, report_path, plan_path, validator)
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["structured_status"], "rejected")
        self.assertEqual(result["exit_code"], 2)
        self.assertIn("max_candidates", result["rejection_reasons"])
        self.assertEqual(len(result["candidates"]), 168)
        self.assertEqual(result["candidate_validation"], [])
        self.assertEqual(result["candidate_simulations_launched"], 0)
        self.assertEqual(result["study_evaluate_invocations"], 0)
        self.assertEqual(invocations, [])

    def test_preflight_cli_exit_matches_structured_over_cap_status(self) -> None:
        manifest = copy.deepcopy(self.final_manifest)
        manifest["limits"]["max_candidates"] = 167
        manifest_path, plan_path, report_path = self._write_fixture_paths(manifest)
        process = subprocess.run([
            sys.executable,
            str(self.root / "experiments/run_budget_constrained_study.py"),
            "preflight",
            "--manifest", str(manifest_path),
            "--binary", str(self.binary),
            "--plan-report", str(plan_path),
            "--report", str(report_path),
        ], cwd=self.root, text=True, capture_output=True, check=False)
        record = self.runner.load_json(report_path)
        self.assertEqual(process.returncode, 2, process.stderr)
        self.assertEqual(record["structured_status"], "rejected")
        self.assertEqual(record["exit_code"], process.returncode)
        self.assertIn("max_candidates", record["rejection_reasons"])
        self.assertEqual(record["candidate_simulations_launched"], 0)
        self.assertEqual(record["study_evaluate_invocations"], 0)

    def test_preflight_ready_report_validates_all_candidates_without_simulating(self) -> None:
        manifest_path, plan_path, report_path = self._write_fixture_paths(self.final_manifest)
        invocations: list[list[str]] = []

        def validator(command: list[str], _cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
            invocations.append(command)
            return subprocess.CompletedProcess(command, 0, "Config is valid: candidate\n", "")

        result = self.runner.preflight(self.root, manifest_path, self.binary, report_path, plan_path, validator)
        self.assertEqual(result["status"], "ready")
        self.assertEqual(result["structured_status"], "ready")
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["candidate_count"], 168)
        self.assertTrue(all(check["pass"] for check in result["cap_checks"].values()))
        self.assertEqual(len(result["candidates"]), 168)
        self.assertTrue(all(row["configuration_validation_status"] == "valid" for row in result["candidates"]))
        self.assertEqual(result["candidate_simulations_launched"], 0)
        self.assertEqual(result["study_evaluate_invocations"], 0)
        self.assertTrue(invocations)
        self.assertTrue(all(command[1] == "validate" for command in invocations))
        self.assertGreater(result["estimate"]["candidate_request_bytes_exact_worst_case"], 0)
        self.assertEqual(
            result["estimate"]["candidate_request_bytes_exact_worst_case"],
            result["estimate"]["first_pass_request_bytes_exact"]
            + result["estimate"]["worst_case_trace_replay_request_bytes_exact"],
        )

    def test_m3_request_projection_is_schema_shaped_and_byte_exact(self) -> None:
        hashes = self.runner.calculate_source_hashes(self.root, self.runner.source_paths(self.final_manifest))
        build_id = self.runner.file_sha256(self.binary)
        candidates, _ = self.runner.enumerate_candidates(self.final_manifest, build_id, hashes)
        request_sizes = []
        for candidate in candidates:
            request = self.runner.build_candidate_request(self.final_manifest, candidate, hashes, False)
            self.assertEqual(request["schema_id"], "axissim.study-candidate-request.v1")
            self.assertEqual(set(request), {
                "schema_id", "manifest_id", "candidate_id", "simulator_build_identity",
                "configuration_identity", "workload_identity", "model_assumption_identity",
                "measurement_protocol_identity", "configuration", "workload", "measurement_protocol",
                "limits", "power_overlay", "factor_coverage", "trace_policy",
            })
            factors = request["factor_coverage"]
            factor_ids = [item["factor_id"] for item in factors]
            self.assertEqual(factor_ids, sorted(factor_ids))
            self.assertEqual(set(factor_ids), {item["factor_id"] for item in candidate["factor_assignments"]})
            self.assertEqual(request["configuration"]["expected_it_power_budget_w"], Decimal("0.23"))
            self.assertFalse(request["trace_policy"]["retain_full_trace"])
            self.assertEqual(request["candidate_id"], candidate["candidate_id"])
            expected_candidate_id = self.runner.digest({
                "manifest_id": request["manifest_id"],
                "simulator_build_identity": request["simulator_build_identity"],
                "workload_identity": request["workload_identity"],
                "configuration_identity": request["configuration_identity"],
                "model_assumption_identity": request["model_assumption_identity"],
                "measurement_protocol_identity": request["measurement_protocol_identity"],
                "factor_assignments": candidate["factor_assignments"],
            })
            self.assertEqual(expected_candidate_id, request["candidate_id"])
            request_sizes.append(len(self.runner.canonical_json(request)))
            trace_request = self.runner.build_candidate_request(self.final_manifest, candidate, hashes, True)
            self.assertTrue(trace_request["trace_policy"]["retain_full_trace"])
            self.assertEqual(
                len(self.runner.canonical_json(trace_request)) - len(self.runner.canonical_json(request)),
                len(b'"retain_full_trace":true') - len(b'"retain_full_trace":false'),
            )
        self.assertEqual(sum(request_sizes), sum(
            len(self.runner.canonical_json(self.runner.build_candidate_request(
                self.final_manifest, candidate, hashes, False)))
            for candidate in candidates
        ))

    def test_process_exit_and_structured_json_status_are_both_required(self) -> None:
        accepts = self.runner.evaluator_outcome_is_accepted
        self.assertTrue(accepts(0, "completed"))
        self.assertFalse(accepts(1, "completed"))
        self.assertFalse(accepts(0, "failed"))
        self.assertFalse(accepts(0, "censored"))

    def test_cpp_failure_result_and_process_exit_status_agree(self) -> None:
        module_path = self.root / "tests/study_contract_e2e.py"
        spec = importlib.util.spec_from_file_location("axissim_study_contract_e2e", module_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.CandidateApiTests.root = self.root
        module.CandidateApiTests.binary = self.binary
        module.CandidateApiTests.build_identity = self.runner.file_sha256(self.binary)
        test = module.CandidateApiTests("test_censored_candidate_and_result_size_fallback")
        test.setUp()
        try:
            # This replays the existing M3 failure contract: event-censored and
            # result-size-fallback calls must return nonzero and encode the same
            # censored/failed status in their versioned JSON result.
            test.test_censored_candidate_and_result_size_fallback()
        finally:
            test.tearDown()

    def test_preflight_command_exit_and_structured_rejection_agree(self) -> None:
        manifest = copy.deepcopy(self.final_manifest)
        manifest["limits"]["max_candidates"] = 1
        manifest_path, plan_path, report_path = self._write_fixture_paths(manifest)
        relative_manifest = manifest_path.relative_to(self.root).as_posix()
        relative_report = report_path.relative_to(self.root).as_posix()
        process = subprocess.run(
            [sys.executable, str(self.root / "experiments/run_budget_constrained_study.py"),
             "preflight", "--manifest", relative_manifest, "--binary", self.binary.relative_to(self.root).as_posix(),
             "--plan-report", plan_path.relative_to(self.root).as_posix(), "--report", relative_report],
            cwd=self.root, capture_output=True, text=True, check=False,
        )
        report = self.runner.load_json(report_path)
        self.assertEqual(process.returncode, 2)
        self.assertEqual(report["structured_status"], "rejected")
        self.assertEqual(report["exit_code"], process.returncode)
        self.assertEqual(report["candidate_simulations_launched"], 0)
        self.assertIn("max_candidates", report["rejection_reasons"])

    def test_candidate_configuration_validation_checks_exit_and_result_text(self) -> None:
        hashes = self.runner.calculate_source_hashes(self.root, self.runner.source_paths(self.final_manifest))
        candidates, _ = self.runner.enumerate_candidates(self.final_manifest, self.runner.file_sha256(self.binary), hashes)
        calls = 0

        def validator(command: list[str], _cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
            nonlocal calls
            calls += 1
            if calls == 1:
                return subprocess.CompletedProcess(command, 0, "not a valid response\n", "")
            return subprocess.CompletedProcess(command, 1, "Config is valid: misleading\n", "bad exit")

        rows = self.runner.validate_candidate_configs(self.root, self.binary, candidates, validator)
        self.assertTrue(rows)
        self.assertEqual(rows[0]["status"], "invalid")
        self.assertEqual(rows[0]["return_code"], 0)
        self.assertEqual(rows[0]["structured_status"], "invalid")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
