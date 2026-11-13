#!/usr/bin/env python3
"""M5 execution boundary, result-integrity, and Pareto tests."""

from __future__ import annotations

import argparse
import copy
import csv
import importlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from decimal import Decimal

TEST_SOURCE_ROOT: pathlib.Path | None = None
TEST_BINARY: pathlib.Path | None = None


def load_runner(source_root: pathlib.Path):
    sys.path.insert(0, str(source_root / "experiments"))
    return importlib.import_module("run_budget_constrained_study")


class StudyM5Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if TEST_SOURCE_ROOT is None or TEST_BINARY is None:
            raise RuntimeError("test paths were not configured")
        cls.root = TEST_SOURCE_ROOT.resolve()
        cls.binary = TEST_BINARY.resolve()
        cls.runner = load_runner(cls.root)
        cls.manifest = cls.runner.load_json(cls.root / "experiments/budget_constrained_study/manifest.json")
        cls.source_hashes = cls.runner.calculate_source_hashes(cls.root, cls.runner.source_paths(cls.manifest))
        cls.build_identity = cls.runner.file_sha256(cls.binary)
        cls.candidates, _ = cls.runner.enumerate_candidates(cls.manifest, cls.build_identity, cls.source_hashes)
        cls.baseline_ids = cls.runner.baseline_candidate_ids(cls.manifest, cls.candidates)

    def setUp(self) -> None:
        self.runner = self.__class__.runner
        self.root = self.__class__.root
        self.binary = self.__class__.binary
        self.manifest = self.__class__.manifest
        self.source_hashes = self.__class__.source_hashes
        self.candidates = self.__class__.candidates
        self.baseline_ids = self.__class__.baseline_ids

    def _run_baselines(self, output: pathlib.Path, process_runner=None):
        return self.runner._run_candidate_set(
            self.root, self.binary, output, self.manifest, self.candidates, self.baseline_ids,
            self.source_hashes, 128 * 1024 * 1024,
            int(self.manifest["limits"]["max_candidate_result_bytes"]), 1024 * 1024,
            process_runner)

    def test_actual_three_workload_execution_repeats_canonically(self) -> None:
        with tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-contract-a-") as first_dir, \
             tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-contract-b-") as second_dir:
            first = self._run_baselines(pathlib.Path(first_dir))
            second = self._run_baselines(pathlib.Path(second_dir))
            first_results, first_requests = first[0], first[1]
            second_results = second[0]
            for candidate_id in self.baseline_ids:
                result_a = first_results[candidate_id]
                result_b = second_results[candidate_id]
                self.assertEqual(result_a["execution_status"], "completed")
                self.assertEqual(result_a["process_return_code"], 0)
                self.assertEqual(result_a["power_budget_status"], "pass")
                self.assertEqual(result_a["configured_it_power_budget_w"], Decimal("0.23000000000000001"))
                self.assertEqual(self.runner.canonical_json(result_a), self.runner.canonical_json(result_b))
                request = first_requests[candidate_id]
                self.assertEqual(result_a["candidate_id"], request["candidate_id"])
                self.assertIn("measurements", result_a)
                self.assertEqual(result_a["power_accounting_coverage"]["schema_id"], "axissim.power-coverage.v1")
            candidate = next(item for item in self.candidates if item["candidate_id"] == self.baseline_ids[0])
            workload = next(item for item in self.manifest["workloads"]
                            if item["workload_id"] == candidate["workload_id"])
            report, markdown = self.runner._workload_report(
                workload, [candidate], {candidate["candidate_id"]: first_results[candidate["candidate_id"]]},
                candidate["candidate_id"], [], [], {}, False)
            self.assertEqual(report["energy_metric_scope"]["baseline_denominator_count"], 30)
            self.assertIn("frames plus control units", report["energy_metric_scope"]["denominator"])
            self.assertIn("Energy / completed work unit (J)", markdown)
            self.assertIn("energy / frame", markdown)
            valid_result = self.runner.load_json(pathlib.Path(first_dir) / "results" / f"{self.baseline_ids[0]}.json")
            bad_request = copy.deepcopy(first_requests[self.baseline_ids[0]])
            bad_request["candidate_id"] = "0" * 64
            with self.assertRaises(self.runner.StudyError):
                self.runner.validate_candidate_result(valid_result, bad_request, 0,
                                                      len(self.runner.canonical_json(valid_result)), 65536)

    def test_candidate_wall_timing_excludes_study_orchestration_and_handles_trace_replay(self) -> None:
        timing = self.runner._candidate_wall_timing_summary(
            Decimal("2.5"),
            [{"elapsed_wall_seconds": Decimal("1.25")}, {"elapsed_wall_seconds": Decimal("0.75")}],
            [{"elapsed_wall_seconds": Decimal("1.5")}],
        )
        self.assertEqual(timing["actual_first_pass_candidate_wall_seconds"], Decimal("2.00"))
        self.assertEqual(timing["actual_trace_replay_candidate_wall_seconds"], Decimal("1.5"))
        self.assertEqual(timing["actual_candidate_wall_seconds_total"], Decimal("3.50"))
        self.assertEqual(timing["candidate_wall_underprediction_ratio"], Decimal("1.4"))
        self.assertIn("excludes Python orchestration", timing["candidate_wall_timing_basis"])

    def test_nonzero_exit_mismatch_is_isolated_and_timeout_is_censored(self) -> None:
        failing_id = self.baseline_ids[0]

        def nonzero_first(command, **kwargs):
            process = subprocess.run(command, **kwargs)
            request_path = pathlib.Path(command[command.index("--request") + 1])
            request = json.loads(request_path.read_text(encoding="utf-8"))
            if request["candidate_id"] == failing_id:
                return subprocess.CompletedProcess(command, 19, process.stdout, process.stderr)
            return process

        with tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-isolation-") as temp_dir:
            result_map, requests, _, _, _ = self._run_baselines(pathlib.Path(temp_dir), nonzero_first)
            failure = result_map[failing_id]
            self.assertEqual(failure["schema_id"], self.runner.RUNNER_FAILURE_SCHEMA)
            self.assertEqual(failure["runner_status"], "failed")
            self.assertEqual(failure["failure_reason"], "evaluator_crash")
            self.assertEqual(failure["process_return_code"], 19)
            self.assertIsNone(failure.get("measurements"))
            failure_file = json.loads((pathlib.Path(temp_dir) / "results" / f"{failing_id}.json").read_text())
            self.assertEqual(set(failure_file), {"schema_id", "candidate_id", "simulator_build_identity",
                                                 "runner_status", "failure_reason", "message"})
            for candidate_id in self.baseline_ids[1:]:
                self.assertEqual(result_map[candidate_id]["execution_status"], "completed")

            request_path = pathlib.Path(temp_dir) / "timeout-request.json"
            request_path.write_bytes(self.runner.canonical_json(requests[failing_id]))
            result_path = pathlib.Path(temp_dir) / "timeout-result.json"

            def timeout_runner(command, **kwargs):
                raise subprocess.TimeoutExpired(command, kwargs.get("timeout", 1))

            timed_out, _ = self.runner.invoke_candidate(
                self.root, self.binary, request_path, result_path, None, 0.01,
                failing_id, self.runner.file_sha256(self.binary),
                int(self.manifest["limits"]["max_candidate_result_bytes"]), timeout_runner)
            self.assertEqual(timed_out["schema_id"], self.runner.RUNNER_FAILURE_SCHEMA)
            self.assertEqual(timed_out["runner_status"], "censored")
            self.assertEqual(timed_out["failure_reason"], "candidate_wall_timeout")
            self.assertFalse(result_path.exists())

    def test_trace_validation_and_trace_cap(self) -> None:
        candidate_id = self.baseline_ids[0]
        candidate = next(item for item in self.candidates if item["candidate_id"] == candidate_id)
        request = self.runner.build_candidate_request(self.manifest, candidate, self.source_hashes, True)
        request["trace_policy"]["max_trace_bytes"] = int(self.manifest["trace_policy"]["max_trace_bytes"])
        with tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-trace-") as temp_dir:
            directory = pathlib.Path(temp_dir)
            request_path = directory / "request.json"
            result_path = directory / "result.json"
            trace_path = directory / "trace.jsonl"
            request_path.write_bytes(self.runner.canonical_json(request))
            result, _ = self.runner.invoke_candidate(
                self.root, self.binary, request_path, result_path, trace_path, 30,
                candidate_id, candidate["simulator_build_identity"],
                int(self.manifest["limits"]["max_candidate_result_bytes"]))
            valid, reason, raw = self.runner.validate_study_trace(
                trace_path, request, result, int(self.manifest["trace_policy"]["max_trace_bytes"]))
            self.assertTrue(valid, reason)
            self.assertIsNotNone(raw)

            tiny_request = copy.deepcopy(request)
            tiny_request["trace_policy"]["max_trace_bytes"] = 1
            tiny_request_path = directory / "tiny-request.json"
            tiny_request_path.write_bytes(self.runner.canonical_json(tiny_request))
            tiny_result_path = directory / "tiny-result.json"
            tiny_trace_path = directory / "tiny-trace.jsonl"
            capped, _ = self.runner.invoke_candidate(
                self.root, self.binary, tiny_request_path, tiny_result_path, tiny_trace_path, 30,
                candidate_id, candidate["simulator_build_identity"],
                int(self.manifest["limits"]["max_candidate_result_bytes"]))
            self.assertEqual(capped["execution_status"], "completed")
            self.assertEqual(capped["trace_export"]["status"], "omitted_limit")
            self.assertFalse(tiny_trace_path.exists())

    def test_pareto_directions_ties_integer_and_tolerance(self) -> None:
        objectives = [
            {"objective_id": "latency", "direction": "minimize", "kind": "numeric"},
            {"objective_id": "energy", "direction": "minimize", "kind": "numeric"},
            {"objective_id": "throughput", "direction": "maximize", "kind": "numeric"},
            {"objective_id": "misses", "direction": "minimize", "kind": "integer"},
        ]

        def point(name, latency, energy, throughput, misses):
            return {"candidate_id": name, "objective_values": {
                "latency": Decimal(str(latency)), "energy": Decimal(str(energy)),
                "throughput": Decimal(str(throughput)), "misses": misses}}

        points = [point("a", 10, 5, 10, 2),
                  point("b", 10.0000000005, 5, 10, 2),  # within the configured 1e-9 tolerance
                  point("c", 9, 6, 12, 1),              # latency/throughput gain, energy tradeoff
                  point("d", 11, 6, 9, 3),              # dominated by a
                  point("e", 10, 5, 10, 2)]
        frontier = self.runner.pareto_frontier(points, objectives)
        self.assertEqual([item["candidate_id"] for item in frontier], ["a", "b", "c", "e"])
        self.assertTrue(self.runner.dominates(points[0], points[3], objectives))
        self.assertFalse(self.runner.dominates(points[0], points[1], objectives))
        self.assertFalse(self.runner.dominates(points[0], points[2], objectives))
        self.assertEqual(self.runner.metric_equivalence_classes(frontier, objectives), [["a", "b", "e"]])

    def test_shared_capacity_pareto_fixture_matches_legacy_filter(self) -> None:
        fixture_path = self.root / "tests/fixtures/pareto_capacity_sweep.csv"
        with fixture_path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        feasible = [row for row in rows if row["feasible"] == "true"]
        points = [{"candidate_id": row["mapping"], "objective_values": {
            "latency": Decimal(row["latency_us"]),
            "energy": Decimal(row["it_energy_per_frame_uj"]),
        }} for row in feasible]
        objectives = [
            {"objective_id": "latency", "direction": "minimize", "kind": "numeric"},
            {"objective_id": "energy", "direction": "minimize", "kind": "numeric"},
        ]
        frontier = self.runner.pareto_frontier(points, objectives)
        self.assertEqual([row["candidate_id"] for row in frontier], ["a", "b"])

    def test_bottleneck_diagnostics_reports_model_sensitivity_not_physical_attribution(self) -> None:
        observations = [
            {"comparison_class": "model_sensitivity", "tested_factors": ["arithmetic_scale"],
             "manifest_group_id": "model", "release_stratum_id": "finite",
             "min_frame_mean_latency_us": Decimal(1), "max_frame_mean_latency_us": Decimal(1000)},
            {"comparison_class": "architecture", "tested_factors": ["rvv_lanes"],
             "manifest_group_id": "oat:rvv_lanes", "release_stratum_id": "finite",
             "min_frame_mean_latency_us": Decimal(7), "max_frame_mean_latency_us": Decimal(10)},
            {"comparison_class": "architecture", "tested_factors": ["dram_bandwidth_gbps"],
             "manifest_group_id": "oat:dram_bandwidth_gbps", "release_stratum_id": "finite",
             "min_frame_mean_latency_us": Decimal(10), "max_frame_mean_latency_us": Decimal(100),
             "recommendation_scope": "exploratory_missing_costs",
             "power_coverage_scope": "swept_factors_only"},
        ]
        rows = []
        for macs, low, high in ((512, 100, 10), (1024, 99, 9)):
            rows.extend([
                {"swept_values": {"npu_macs": macs, "dram_bandwidth_gbps": 0.5},
                 "metrics": {"frame_mean_latency_us": Decimal(low)},
                 "recommendation_scope": "exploratory_missing_costs"},
                {"swept_values": {"npu_macs": macs, "dram_bandwidth_gbps": 20},
                 "metrics": {"frame_mean_latency_us": Decimal(high)},
                 "recommendation_scope": "exploratory_missing_costs"},
            ])
        diagnostic = self.runner.bottleneck_diagnostics(observations, [{
            "manifest_group_id": "interaction:npu_macs_x_dram_bandwidth",
            "release_stratum_id": "finite", "candidate_rows": rows,
        }])
        self.assertEqual(diagnostic["largest_single_factor_latency_sensitivity"]["manifest_group_id"],
                         "oat:dram_bandwidth_gbps")
        self.assertEqual(diagnostic["largest_single_factor_latency_sensitivity"]["power_coverage_scope"],
                         "swept_factors_only")
        interaction = diagnostic["compute_memory_interactions"][0]
        self.assertTrue(interaction["complete_factor_grid"])
        self.assertEqual(interaction["maximum_bandwidth_latency_span_us"], Decimal(90))
        self.assertEqual(interaction["maximum_mac_latency_span_us"], Decimal(1))
        self.assertEqual(interaction["recommendation_scope"], "exploratory_missing_costs")
        self.assertEqual(interaction["power_coverage_scope"], "swept_factors_only")
        self.assertIn("not a calibrated physical bottleneck", interaction["interpretation"])

    def test_eligibility_coverage_signature_and_output_cap(self) -> None:
        request = self.runner.build_candidate_request(
            self.manifest, next(item for item in self.candidates if item["candidate_id"] == self.baseline_ids[0]),
            self.source_hashes, False)
        result = {
            "execution_status": "completed", "power_budget_status": "pass",
            "operating_state": "no_growth_observed",
            "power_accounting_coverage": {"schema_id": "axissim.power-coverage.v1", "factors": [
                {"factor_id": "dram_bandwidth_gbps", "applicability": "modeled", "power_coverage": "partial",
                 "represented_terms": ["static.dram_interface_w"],
                 "omitted_terms": ["missing.static.dram_interface_w_per_gbps"]}]},
            "measurements": {
                "interval": {"complete": True},
                "work_units": {"scheduled_target_cohort": {"frame": 1, "control": 0, "total": 1},
                               "incomplete_target_cohort": {"total": 0}},
                "latency": {"frame": {"mean_us": Decimal(1), "p99_us": Decimal(1)}, "control": {}},
                "deadline": {"frame": {"miss_count": 0}, "control": {}},
                "energy": {"energy_per_completed_work_unit_j": Decimal("0.1")},
                "throughput_work_units_per_second": Decimal(10)},
        }
        objectives, values, deadline_ok = self.runner.candidate_objectives(result, request)
        self.assertTrue(self.runner.result_is_frontier_eligible(result, objectives, values, deadline_ok)[0])
        entries, signature, scope = self.runner.coverage_for_group(result, ["dram_bandwidth_gbps"])
        self.assertEqual(entries, ["missing.static.dram_interface_w_per_gbps"])
        self.assertEqual(signature, self.runner.digest(entries))
        self.assertEqual(scope, "exploratory_missing_costs")
        censored = copy.deepcopy(result)
        censored["execution_status"] = "censored"
        self.assertFalse(self.runner.result_is_frontier_eligible(censored, objectives, values, deadline_ok)[0])
        growing = copy.deepcopy(result)
        growing["operating_state"] = "backlog_growing"
        self.assertFalse(self.runner.result_is_frontier_eligible(growing, objectives, values, deadline_ok)[0])
        over_budget = copy.deepcopy(result)
        over_budget["power_budget_status"] = "fail"
        self.assertFalse(self.runner.result_is_frontier_eligible(over_budget, objectives, values, deadline_ok)[0])

        with tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-cap-") as temp_dir:
            output_root = pathlib.Path(temp_dir)
            content = b"12345"
            accepted = self.runner._write_output_file(output_root, output_root / "kept.bin", content, 8)
            rejected = self.runner._write_output_file(output_root, output_root / "too-large.bin", content, 8)
            self.assertTrue(accepted)
            self.assertFalse(rejected)
            self.assertEqual((output_root / "kept.bin").read_bytes(), content)
            self.assertFalse((output_root / "too-large.bin").exists())

    def test_non_applicable_parameter_never_gets_a_frontier(self) -> None:
        candidate = next(item for item in self.candidates
                         if item["workload_id"] == "mnist12_profiled"
                         and any(member["group_id"] == "oat:sram_capacity_bytes:mnist"
                                 for member in item["group_memberships"]))
        candidate_id = candidate["candidate_id"]
        request = self.runner.build_candidate_request(self.manifest, candidate, self.source_hashes, False)
        result = {
            "candidate_id": candidate_id, "execution_status": "completed", "power_budget_status": "pass",
            "operating_state": "not_applicable",
            "power_accounting_coverage": {"schema_id": "axissim.power-coverage.v1", "factors": [
                {"factor_id": "sram_capacity_bytes", "applicability": "n/a", "power_coverage": "n/a",
                 "represented_terms": [], "omitted_terms": []}]},
            "measurements": {
                "interval": {"complete": True},
                "work_units": {"scheduled_target_cohort": {"frame": 1, "control": 0, "total": 1},
                               "incomplete_target_cohort": {"total": 0}},
                "latency": {"frame": {"mean_us": Decimal(10), "p99_us": Decimal(10)}, "control": {}},
                "deadline": {"frame": {"miss_count": 0}, "control": {}},
                "energy": {"energy_per_completed_work_unit_j": Decimal("0.1")},
                "throughput_work_units_per_second": Decimal(10)},
        }
        groups, comparisons, frontier = self.runner.build_comparison_report(
            self.manifest, [candidate], {candidate_id: result}, {candidate_id: request})
        target = next(group for group in comparisons
                      if group["manifest_group_id"] == "oat:sram_capacity_bytes:mnist")
        self.assertEqual(target["recommendation_scope"], "not_applicable")
        self.assertEqual(target["power_coverage_scope"], "swept_factors_not_applicable")
        self.assertEqual(target["frontier_status"], "not_applicable_no_frontier")
        self.assertEqual(target["frontier_candidate_ids"], [])
        self.assertNotIn(candidate_id, frontier)

    def test_contract_and_preflight_provenance_mismatch_rejected(self) -> None:
        candidate_id = self.baseline_ids[0]
        candidate = next(item for item in self.candidates if item["candidate_id"] == candidate_id)
        request = self.runner.build_candidate_request(self.manifest, candidate, self.source_hashes, False)
        bad_request = copy.deepcopy(request)
        bad_request["candidate_id"] = "0" * 64
        with self.assertRaises(self.runner.StudyError):
            self.runner.validate_candidate_result({}, bad_request, 0, 2, 65536)

        with tempfile.TemporaryDirectory(dir=self.root / "build", prefix="m5-provenance-") as temp_dir:
            directory = pathlib.Path(temp_dir)
            manifest = copy.deepcopy(self.manifest)
            manifest["planning_report_path"] = "build/m5-provenance-plan.json"
            manifest_path = directory / "manifest.json"
            manifest_path.write_bytes(self.runner.canonical_json(manifest))
            plan_path = self.root / manifest["planning_report_path"]
            plan = {"schema_id": "axissim.study-plan-report.v1", "status": "planned",
                    "plan_id": "a" * 64, "simulator_build_identity": self.runner.file_sha256(self.binary),
                    "source_hashes": self.source_hashes}
            plan_path.write_bytes(self.runner.canonical_json(plan))
            preflight = {"schema_id": "axissim.study-preflight-report.v1", "status": "ready",
                         "structured_status": "ready", "manifest_identity": "f" * 64,
                         "simulator_build_identity": self.runner.file_sha256(self.binary),
                         "planning_report_identity": "a" * 64}
            preflight_path = directory / "preflight.json"
            preflight_path.write_bytes(self.runner.canonical_json(preflight))
            with self.assertRaises(self.runner.StudyError):
                self.runner._verify_execution_inputs(
                    self.root, manifest_path, self.binary, plan_path, preflight_path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    known, remainder = parser.parse_known_args()
    TEST_SOURCE_ROOT = known.source_root
    TEST_BINARY = known.binary
    unittest.main(argv=[sys.argv[0], *remainder])
