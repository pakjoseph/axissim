#!/usr/bin/env python3
"""End-to-end checks for the versioned C++ study-evaluate process boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unicodedata
import unittest
from decimal import Decimal


def canonical(value: object) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, Decimal):
        if not value.is_finite():
            raise ValueError("non-finite canonical number")
        normalized = value.normalize()
        if normalized == 0:
            return "0"
        return format(normalized, "f")
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return canonical(Decimal(str(value)))
    if isinstance(value, str):
        return json.dumps(unicodedata.normalize("NFC", value), ensure_ascii=False, separators=(",", ":"))
    if isinstance(value, list):
        return "[" + ",".join(canonical(item) for item in value) + "]"
    if isinstance(value, dict):
        normalized: dict[str, object] = {}
        for key, item in value.items():
            key = unicodedata.normalize("NFC", key)
            if key in normalized:
                raise ValueError("canonical key collision")
            normalized[key] = item
        parts = []
        for key in sorted(normalized, key=lambda item: item.encode("utf-8")):
            parts.append(canonical(key) + ":" + canonical(normalized[key]))
        return "{" + ",".join(parts) + "}"
    raise TypeError(type(value))


def digest(value: object) -> str:
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


def file_hash(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_cli_result(process: subprocess.CompletedProcess[str], result_path: pathlib.Path) -> dict:
    if not result_path.exists():
        raise AssertionError(f"candidate result missing; exit={process.returncode}; stderr={process.stderr}")
    return json.loads(result_path.read_text(encoding="utf-8"))


class CandidateApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        pass

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="axissim-study-contract-")
        self.out = pathlib.Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def invoke(self, request: dict, label: str = "candidate", trace: bool = False) -> tuple[subprocess.CompletedProcess[str], dict, pathlib.Path]:
        request_path = self.out / f"{label}.request.json"
        result_path = self.out / f"{label}.result.json"
        request_path.write_text(canonical(request), encoding="utf-8")
        command = [str(self.binary), "study-evaluate", "--request", str(request_path), "--result", str(result_path)]
        trace_path = self.out / f"{label}.trace.jsonl"
        if trace:
            command.extend(["--trace", str(trace_path)])
        process = subprocess.run(command, cwd=self.root, text=True, capture_output=True, check=False)
        return process, parse_cli_result(process, result_path), trace_path

    def request(self, workload: str = "physical", frames: int = 2, period: Decimal = Decimal("33333"),
                sustained: bool = False, trace: bool = False) -> dict:
        if workload == "physical":
            base = "configs/power_budget/manifest.conf"
            includes = [f"configs/baseline/{name}.conf" for name in ("workload", "architecture", "implementation", "energy")]
            profile_path = mapping_path = None
            profile_hash = mapping_hash = None
            workload_id = "physical_ai_baseline"
            mode = "physical_ai"
            overlay = {"source_path": base, "source_sha256": file_hash(self.root / base), "fields": {}}
        else:
            base = "configs/workloads/mnist12_profiled.conf"
            includes = [f"configs/baseline/{name}.conf" for name in ("architecture", "implementation", "energy")]
            profile_path = "reference_data/workloads/mnist12_profile.json"
            mapping_path = "configs/mappings/mnist12_baseline.json"
            profile_hash = file_hash(self.root / profile_path)
            mapping_hash = file_hash(self.root / mapping_path)
            workload_id = "mnist12"
            mode = "profiled_graph"
            overlay_path = "configs/power_budget/manifest.conf"
            overlay = {"source_path": overlay_path, "source_sha256": file_hash(self.root / overlay_path), "fields": {
                "static_power_w": Decimal("0.05"), "npu_static_power_per_mac_w": Decimal("0.00003"),
                "rvv_static_power_per_lane_w": Decimal("0.002"), "sram_static_power_per_kib_w": Decimal("0.0001"),
                "dram_interface_static_power_w": Decimal("0.02"), "it_power_budget_w": Decimal("0.23")}}

        overrides: dict[str, object] = {"frames": frames}
        if period != Decimal("33333"):
            overrides["frame_period_us"] = period
        configuration = {"base_config_path": base, "base_config_sha256": file_hash(self.root / base),
            "included_config_hashes": {name: file_hash(self.root / name) for name in includes},
            "overrides": overrides, "expected_it_power_budget_w": Decimal("0.23")}
        release_schedule = {"kind": "periodic", "period_us": period}
        protocol: dict[str, object] = {"mode": "finite", "time_origin_us": 0,
            "deadline_policy": {"frame": {"relative_deadline_us": Decimal("33333"), "handling": "objective"},
                                "control": {"relative_deadline_us": None, "handling": "not_applicable"}},
            "percentile_method": "linear_n_minus_1_p99"}
        if sustained:
            protocol.update({"mode": "sustained", "warmup_end_us": Decimal("100"),
                "window_start_us": Decimal("100"), "window_end_us": Decimal("300")})
        power_overlay = overlay
        factor_records = [{"factor_id": factor_id, "treatment": "held_constant",
            "evidence_source": "checked-in configuration", "rationale": "Held constant in this contract fixture."}
            for factor_id in ("dram_bandwidth_gbps", "npu_macs", "sram_capacity_bytes")]
        manifest_id = digest({"test": "AxisSim candidate API"})
        request = {"schema_id": "axissim.study-candidate-request.v1", "manifest_id": manifest_id,
            "candidate_id": "0" * 64, "simulator_build_identity": self.build_identity,
            "configuration_identity": "0" * 64, "workload_identity": "0" * 64,
            "model_assumption_identity": "0" * 64, "measurement_protocol_identity": "0" * 64,
            "configuration": configuration,
            "workload": {"workload_id": workload_id, "workload_mode": mode, "profile_path": profile_path,
                "profile_sha256": profile_hash, "mapping_path": mapping_path, "mapping_sha256": mapping_hash,
                "release_stratum_id": "periodic", "release_schedule": release_schedule},
            "measurement_protocol": protocol,
            "limits": {"max_candidates": 20, "max_task_events_per_candidate": 100000,
                "max_backlog_work_units": 1000, "max_retained_trace_bytes": 1000000,
                "max_candidate_result_bytes": 1000000, "max_study_output_bytes": 4000000,
                "max_simulated_time_us": Decimal("1000000"), "max_drain_simulated_us": Decimal("1000000"),
                "candidate_wall_timeout_seconds": Decimal("10")},
            "power_overlay": power_overlay, "factor_coverage": factor_records,
            "trace_policy": {"retain_full_trace": trace, "max_trace_bytes": 1000000}}
        # The runner owns digest construction. This fixture constructs identities
        # independently using the documented canonical payloads.
        request["configuration_identity"] = digest({k: configuration[k] for k in
            ("base_config_path", "base_config_sha256", "included_config_hashes", "overrides")})
        request["workload_identity"] = digest({"workload_id": workload_id, "workload_mode": mode,
            "profile_sha256": profile_hash, "mapping_sha256": mapping_hash,
            "release_stratum_id": "periodic", "release_schedule": release_schedule})
        request["model_assumption_identity"] = digest({"accounting_model_id": "axissim-model-accounting-v1",
            "configuration_identity": request["configuration_identity"], "power_overlay": power_overlay})
        request["measurement_protocol_identity"] = digest(protocol)
        factor_assignments = self.factor_assignments(request)
        request["candidate_id"] = digest({"manifest_id": manifest_id, "simulator_build_identity": self.build_identity,
            "workload_identity": request["workload_identity"], "configuration_identity": request["configuration_identity"],
            "model_assumption_identity": request["model_assumption_identity"],
            "measurement_protocol_identity": request["measurement_protocol_identity"],
            "factor_assignments": factor_assignments})
        return request

    def refresh(self, request: dict) -> None:
        config = request["configuration"]
        work = request["workload"]
        request["configuration_identity"] = digest({k: config[k] for k in
            ("base_config_path", "base_config_sha256", "included_config_hashes", "overrides")})
        request["workload_identity"] = digest({"workload_id": work["workload_id"], "workload_mode": work["workload_mode"],
            "profile_sha256": work["profile_sha256"], "mapping_sha256": work["mapping_sha256"],
            "release_stratum_id": work["release_stratum_id"], "release_schedule": work["release_schedule"]})
        request["model_assumption_identity"] = digest({"accounting_model_id": "axissim-model-accounting-v1",
            "configuration_identity": request["configuration_identity"], "power_overlay": request["power_overlay"]})
        request["measurement_protocol_identity"] = digest(request["measurement_protocol"])
        request["candidate_id"] = digest({"manifest_id": request["manifest_id"],
            "simulator_build_identity": request["simulator_build_identity"],
            "workload_identity": request["workload_identity"], "configuration_identity": request["configuration_identity"],
            "model_assumption_identity": request["model_assumption_identity"],
            "measurement_protocol_identity": request["measurement_protocol_identity"],
            "factor_assignments": self.factor_assignments(request)})

    @staticmethod
    def factor_assignments(request: dict) -> list[dict]:
        defaults = {"dram_bandwidth_gbps": Decimal("20"), "npu_macs": 1024, "sram_capacity_bytes": 131072}
        overrides = request["configuration"]["overrides"]
        values = {"dram_bandwidth_gbps": Decimal(str(overrides.get("dram_bandwidth_gbps", defaults["dram_bandwidth_gbps"]))),
            "npu_macs": int(overrides.get("npu_macs", defaults["npu_macs"])),
            "sram_capacity_bytes": int(overrides.get("sram_capacity_bytes", defaults["sram_capacity_bytes"]))}
        return [{"factor_id": factor_id, "value": values[factor_id]} for factor_id in sorted(values)]

    def test_finite_run_repeatability_and_result_shape(self) -> None:
        req = self.request(frames=2)
        first, a, _ = self.invoke(req, "repeat-a")
        second, b, _ = self.invoke(req, "repeat-b")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(a, b)
        self.assertEqual(a["execution_status"], "completed")
        self.assertEqual(a["schema_id"], "axissim.study-candidate-result.v1")
        self.assertEqual(a["configured_it_power_budget_w"], 0.23)
        self.assertIsNone(a["configured_it_power_budget_unavailable_reason"])
        average_power = a["measurements"]["energy"]["average_it_power_w"]
        expected_budget_status = "pass" if average_power <= a["configured_it_power_budget_w"] else "fail"
        self.assertEqual(a["power_budget_status"], expected_budget_status)
        self.assertEqual(a["feasibility_label"],
            "Provisionally feasible under current modeled power terms." if expected_budget_status == "pass" else None)
        self.assertTrue(a["measurements"]["interval"]["complete"])
        self.assertEqual(a["measurements"]["work_units"]["scheduled_target_cohort"]["total"], 2)
        m = a["measurements"]
        energy = m["energy"]
        dynamic_sum = sum(energy["dynamic_by_category_j"].values())
        self.assertAlmostEqual(dynamic_sum + energy["static_j"], energy["total_it_j"], places=15)
        self.assertEqual(a["trace_export"], {"status": "not_requested", "bytes": 0, "record_count": 0,
            "sha256": None, "unavailable_reason": None})
        coverage = a["power_accounting_coverage"]
        self.assertEqual(coverage["schema_id"], "axissim.power-coverage.v1")
        factors = {record["factor_id"]: record for record in coverage["factors"]}
        self.assertEqual(factors["npu_macs"]["power_coverage"], "complete")
        self.assertEqual(factors["dram_bandwidth_gbps"]["power_coverage"], "partial")
        self.assertEqual(factors["sram_capacity_bytes"]["power_coverage"], "partial")

    def test_budget_boundary_uses_reported_cpp_power_and_coverage(self) -> None:
        request = self.request(frames=1)
        request["configuration"]["overrides"].update({
            "static_power_w": Decimal("0.23"), "npu_static_power_per_mac_w": 0,
            "rvv_static_power_per_lane_w": 0, "sram_static_power_per_kib_w": 0,
            "dram_interface_static_power_w": 0, "vector_op_pj": 0, "npu_mac_pj": 0,
            "dram_byte_pj": 0, "dma_byte_pj": 0})
        self.refresh(request)
        process, result, _ = self.invoke(request, "at-budget-boundary")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(result["execution_status"], "completed")
        self.assertAlmostEqual(result["measurements"]["energy"]["average_it_power_w"], 0.23, places=15)
        self.assertEqual(result["power_budget_status"], "pass")
        self.assertEqual(result["feasibility_label"], "Provisionally feasible under current modeled power terms.")

        above = self.request(frames=1)
        above["configuration"]["overrides"].update(request["configuration"]["overrides"])
        above["configuration"]["overrides"]["static_power_w"] = Decimal("0.2301")
        self.refresh(above)
        process, result, _ = self.invoke(above, "above-budget-boundary")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertGreater(result["measurements"]["energy"]["average_it_power_w"], 0.23)
        self.assertEqual(result["power_budget_status"], "fail")
        self.assertIsNone(result["feasibility_label"])

    def test_censored_candidate_and_result_size_fallback(self) -> None:
        capped = self.request(frames=2)
        capped["limits"]["max_task_events_per_candidate"] = 1
        self.refresh(capped)
        process, result, _ = self.invoke(capped, "event-capped")
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(result["execution_status"], "censored")
        self.assertEqual(result["censor_reason"], "task_event_limit")
        self.assertFalse(result["measurements"]["interval"]["complete"])
        self.assertIsNone(result["feasibility_label"])

        complete_request = self.request(frames=1)
        complete_process, complete_result, _ = self.invoke(complete_request, "size-reference")
        self.assertEqual(complete_process.returncode, 0, complete_process.stderr)
        size_cap = len((self.out / "size-reference.result.json").read_bytes()) - 1
        complete_request["limits"]["max_candidate_result_bytes"] = size_cap
        self.refresh(complete_request)
        limited_process, limited_result, _ = self.invoke(complete_request, "size-limited")
        self.assertNotEqual(limited_process.returncode, 0)
        self.assertEqual(limited_result["execution_status"], "failed")
        self.assertEqual(limited_result["measurements"], None)
        self.assertEqual(limited_result["measurements_unavailable_reason"], "candidate_result_limit")
        self.assertEqual(limited_result["errors"][0]["code"], "candidate_result_limit")
        self.assertEqual(limited_result["feasibility_label"], None)

    def test_trace_is_bounded_and_does_not_change_metrics(self) -> None:
        no_trace_process, no_trace, _ = self.invoke(self.request(frames=1), "without-trace")
        self.assertEqual(no_trace_process.returncode, 0, no_trace_process.stderr)
        request = self.request(frames=1, trace=True)
        process, with_trace, trace_path = self.invoke(request, "with-trace", trace=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        trace_record = with_trace["trace_export"]
        self.assertEqual(trace_record["status"], "written")
        raw = trace_path.read_bytes()
        self.assertEqual(len(raw), trace_record["bytes"])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), trace_record["sha256"])
        lines = [json.loads(line) for line in raw.decode("utf-8").splitlines()]
        self.assertEqual(lines[0]["schema_id"], "axissim.study-task-trace.v1")
        self.assertEqual(lines[0]["activity_assumption"], "uniform_over_task_interval")
        self.assertEqual(len(lines) - 1, trace_record["record_count"])
        self.assertTrue(all(row["completion_status"] == "completed" for row in lines[1:]))
        for key in ("measurements", "power_budget_status", "operating_state"):
            self.assertEqual(with_trace[key], no_trace[key])

        request["trace_policy"]["max_trace_bytes"] = 16
        request["limits"]["max_retained_trace_bytes"] = 16
        self.refresh(request)
        process, omitted, omitted_path = self.invoke(request, "trace-omitted", trace=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(omitted["trace_export"]["status"], "omitted_limit")
        self.assertEqual(omitted["trace_export"]["unavailable_reason"], "max_trace_bytes")
        self.assertFalse(omitted_path.exists())
        self.assertEqual(omitted["measurements"], no_trace["measurements"])

    def test_sustained_cohort_and_post_window_drain(self) -> None:
        request = self.request(frames=4, period=Decimal("100"), sustained=True)
        self.refresh(request)
        process, result, _ = self.invoke(request, "sustained")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(result["execution_status"], "completed")
        m = result["measurements"]
        self.assertEqual(m["interval"], {"start_us": 100, "end_us": 300, "duration_us": 200, "complete": True})
        counts = m["work_units"]
        self.assertEqual(counts["scheduled_before_cutoff"]["total"], 3)
        self.assertEqual(counts["scheduled_target_cohort"]["total"], 2)
        self.assertEqual(counts["released_before_window"]["total"], 1)
        self.assertEqual(counts["released_target_cohort"]["total"], 2)
        self.assertEqual(counts["completed_in_window"]["total"], 0)
        self.assertEqual(m["throughput_work_units_per_second"], 0)
        self.assertEqual(m["backlog"]["at_window_start"], 2)
        self.assertEqual(m["backlog"]["at_window_end"], 3)
        self.assertEqual(result["operating_state"], "backlog_growing")
        self.assertIsNone(m["energy"]["energy_per_completed_work_unit_j"])
        self.assertEqual(m["energy"]["energy_per_completed_work_unit_unavailable_reason"], "no_completed_work_units")

    def test_structured_invalid_input_and_source_binding(self) -> None:
        cases = []
        wrong_schema = self.request(frames=1)
        wrong_schema["schema_id"] = "axissim.study-candidate-request.v9"
        cases.append((wrong_schema, "unsupported_schema"))
        unknown = self.request(frames=1)
        unknown["surprise"] = True
        cases.append((unknown, "unknown_field"))
        stale_include = self.request(frames=1)
        stale_include["configuration"]["included_config_hashes"]["configs/baseline/energy.conf"] = "f" * 64
        self.refresh(stale_include)
        cases.append((stale_include, "configuration_source_mismatch"))
        stale_binary = self.request(frames=1)
        stale_binary["simulator_build_identity"] = "a" * 64
        self.refresh(stale_binary)
        cases.append((stale_binary, "identity_mismatch"))
        mismatch_period = self.request(frames=1)
        mismatch_period["workload"]["release_schedule"]["period_us"] = Decimal("17")
        self.refresh(mismatch_period)
        cases.append((mismatch_period, "release_schedule_mismatch"))
        deadline = self.request(frames=1)
        deadline["measurement_protocol"]["deadline_policy"]["frame"]["relative_deadline_us"] = Decimal("17")
        self.refresh(deadline)
        cases.append((deadline, "deadline_config_mismatch"))
        missing_overlay = self.request(frames=1)
        missing_overlay["power_overlay"] = None
        self.refresh(missing_overlay)
        cases.append((missing_overlay, "workload_input_mismatch"))
        bad_budget = self.request(frames=1)
        bad_budget["configuration"]["expected_it_power_budget_w"] = Decimal("0.2300000000005")
        self.refresh(bad_budget)
        cases.append((bad_budget, "study_budget_mismatch"))
        resolved_bad_budget = self.request(frames=1)
        resolved_bad_budget["configuration"]["overrides"]["it_power_budget_w"] = 0
        self.refresh(resolved_bad_budget)
        process, result, _ = self.invoke(resolved_bad_budget, "resolved-budget-mismatch")
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(result["errors"][0]["code"], "study_budget_mismatch")
        self.assertEqual(result["configured_it_power_budget_w"], 0)
        self.assertIsNone(result["configured_it_power_budget_unavailable_reason"])
        self.assertTrue(result["effective_configuration"])
        self.assertIsNone(result["measurements"])
        for index, (request, code) in enumerate(cases):
            process, result, _ = self.invoke(request, f"invalid-{index}")
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(result["execution_status"], "invalid")
            self.assertIsNone(result["measurements"])
            self.assertEqual(result["measurements_unavailable_reason"], "invalid_input")
            self.assertEqual(result["errors"][0]["code"], code)

        malformed_path = self.out / "malformed.json"
        malformed_result = self.out / "malformed-result.json"
        malformed_path.write_text('{"schema_id":', encoding="utf-8")
        process = subprocess.run([str(self.binary), "study-evaluate", "--request", str(malformed_path),
            "--result", str(malformed_result)], cwd=self.root, text=True, capture_output=True, check=False)
        self.assertNotEqual(process.returncode, 0)
        malformed = json.loads(malformed_result.read_text(encoding="utf-8"))
        self.assertEqual(malformed["errors"][0]["code"], "invalid_json")
        self.assertEqual(malformed["execution_status"], "invalid")

    def test_mnist_overlay_preserves_legacy_timing_and_dynamic_energy(self) -> None:
        request = self.request(workload="mnist", frames=2)
        legacy_json = self.out / "mnist-legacy.json"
        legacy_process = subprocess.run([str(self.binary), "run", str(self.root / request["configuration"]["base_config_path"]),
            "--set", "frames=2", "--json", str(legacy_json)], cwd=self.root, text=True, capture_output=True, check=False)
        self.assertEqual(legacy_process.returncode, 0, legacy_process.stderr)
        before = file_hash(self.root / request["configuration"]["base_config_path"])
        process, result, _ = self.invoke(request, "mnist-overlay")
        self.assertEqual(process.returncode, 0, process.stderr)
        legacy = json.loads(legacy_json.read_text(encoding="utf-8"))
        self.assertEqual(result["effective_configuration"]["it_power_budget_w"], 0.23)
        coverage = {record["factor_id"]: record for record in result["power_accounting_coverage"]["factors"]}
        self.assertEqual(coverage["sram_capacity_bytes"]["applicability"], "n/a")
        self.assertEqual(legacy["power"]["budget_w"], 0.0)
        self.assertAlmostEqual(result["measurements"]["latency"]["frame"]["mean_us"], legacy["latency"]["mean_us"], places=5)
        self.assertAlmostEqual(result["measurements"]["latency"]["frame"]["p99_us"], legacy["latency"]["p99_us"], places=5)
        dynamic = result["measurements"]["energy"]["dynamic_by_category_j"]
        for api_name, legacy_name in (("vector", "vector_pj"), ("npu_arithmetic", "npu_arithmetic_pj"),
                                      ("dram", "dram_pj"), ("dma", "dma_pj")):
            self.assertEqual(round(dynamic[api_name] * 1e12, 6), legacy["energy"][legacy_name])
        self.assertEqual(before, file_hash(self.root / request["configuration"]["base_config_path"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    CandidateApiTests.root = pathlib.Path(args.source_root).resolve()
    CandidateApiTests.binary = pathlib.Path(args.binary).resolve()
    CandidateApiTests.build_identity = file_hash(CandidateApiTests.binary)
    unittest.main(argv=[sys.argv[0]])
